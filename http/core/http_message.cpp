#include "http_message.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <strings.h>

using namespace std;

namespace
{
const size_t kSessionTokenBytes = 32;

string encode_hex(const unsigned char *data, size_t len)
{
    static const char kHexChars[] = "0123456789abcdef";
    string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        hex.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
        hex.push_back(kHexChars[data[i] & 0x0F]);
    }
    return hex;
}
}

namespace http_core
{
HttpRequest::HttpRequest()
    : method(GET),
      content_length(0),
      content_length_seen(false),
      keep_alive(false),
      http_1_1(false),
      chunked(false)
{
}

HttpResponse::HttpResponse()
    : status(200),
      title("OK"),
      content_type("text/html"),
      options_response(false)
{
}

RequestContext::RequestContext()
    : mysql(nullptr),
      upload_max_bytes(100 * 1024 * 1024),
      user_storage_quota_bytes(1024L * 1024L * 1024L),
      legacy_compat_enabled(false),
      close_log(0)
{
}

void RequestContext::release_temp_upload(const string &path)
{
    if (!path.empty())
    {
        released_temp_upload_paths.push_back(path);
    }
}

void RequestContext::release_temp_upload(UploadedFile &upload)
{
    if (!upload.temp_path.empty())
    {
        released_temp_upload_paths.push_back(upload.temp_path);
        upload.released = true;
    }
}

const char *method_name(Method method)
{
    switch (method)
    {
    case GET: return "GET";
    case POST: return "POST";
    case HEAD: return "HEAD";
    case PUT: return "PUT";
    case DELETE: return "DELETE";
    case TRACE: return "TRACE";
    case OPTIONS: return "OPTIONS";
    case CONNECT: return "CONNECT";
    case PATH: return "PATCH";
    default: return "UNKNOWN";
    }
}

bool starts_with_ignore_case(const string &text, const char *prefix)
{
    return prefix != nullptr &&
           text.size() >= strlen(prefix) &&
           strncasecmp(text.c_str(), prefix, strlen(prefix)) == 0;
}

bool equals_ignore_case(const string &left, const char *right)
{
    return right != nullptr && strcasecmp(left.c_str(), right) == 0;
}

string lowercase_copy(const string &value)
{
    string lowered;
    lowered.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        lowered.push_back(static_cast<char>(tolower(static_cast<unsigned char>(value[i]))));
    }
    return lowered;
}

bool HttpRequest::is_api_request() const
{
    return starts_with_ignore_case(path, "/api/");
}

bool HttpRequest::requires_auth() const
{
    return starts_with_ignore_case(path, "/api/private/") ||
           starts_with_ignore_case(path, "/api/drive/") ||
           starts_with_ignore_case(path, "/api/admin/");
}

bool HttpRequest::should_skip_request_log() const
{
    return equals_ignore_case(path, "/healthz");
}

const char *HttpRequest::method_name() const
{
    return http_core::method_name(method);
}

string HttpRequest::header_value(const string &key) const
{
    map<string, string>::const_iterator it = headers.find(lowercase_copy(key));
    return it == headers.end() ? "" : it->second;
}

string url_decode(const string &value)
{
    string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '+')
        {
            decoded.push_back(' ');
        }
        else if (value[i] == '%' && i + 2 < value.size())
        {
            char high = value[i + 1];
            char low = value[i + 2];
            if (isxdigit((unsigned char)high) && isxdigit((unsigned char)low))
            {
                char hex[3] = {high, low, '\0'};
                decoded.push_back((char)strtol(hex, nullptr, 16));
                i += 2;
            }
            else
            {
                decoded.push_back(value[i]);
            }
        }
        else
        {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

string trim_copy(const string &value)
{
    size_t start = 0;
    while (start < value.size() && isspace((unsigned char)value[start]))
    {
        ++start;
    }
    size_t end = value.size();
    while (end > start && isspace((unsigned char)value[end - 1]))
    {
        --end;
    }
    return value.substr(start, end - start);
}

string json_escape(const string &value)
{
    string escaped;
    escaped.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        switch (value[i])
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(value[i]) < 0x20)
            {
                escaped += " ";
            }
            else
            {
                escaped.push_back(value[i]);
            }
            break;
        }
    }
    return escaped;
}

bool decode_base64(const string &input, string &output)
{
    string cleaned;
    cleaned.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        unsigned char ch = (unsigned char)input[i];
        if (!isspace(ch))
        {
            cleaned.push_back((char)ch);
        }
    }

    if (cleaned.empty() || cleaned.size() % 4 != 0)
    {
        return false;
    }

    vector<unsigned char> decoded((cleaned.size() / 4) * 3, 0);
    int decoded_len = EVP_DecodeBlock(decoded.data(),
                                      (const unsigned char *)cleaned.data(),
                                      (int)cleaned.size());
    if (decoded_len < 0)
    {
        return false;
    }

    int padding = 0;
    if (!cleaned.empty() && cleaned[cleaned.size() - 1] == '=')
    {
        ++padding;
    }
    if (cleaned.size() > 1 && cleaned[cleaned.size() - 2] == '=')
    {
        ++padding;
    }
    decoded_len -= padding;
    if (decoded_len < 0)
    {
        return false;
    }

    output.assign((const char *)decoded.data(), decoded_len);
    return true;
}

string header_param_value(const string &header_line, const string &key)
{
    string pattern = key + "=";
    size_t pos = header_line.find(pattern);
    if (pos == string::npos)
    {
        return "";
    }

    pos += pattern.size();
    while (pos < header_line.size() && isspace((unsigned char)header_line[pos]))
    {
        ++pos;
    }

    if (pos >= header_line.size())
    {
        return "";
    }

    if (header_line[pos] == '"')
    {
        ++pos;
        size_t end = header_line.find('"', pos);
        if (end == string::npos)
        {
            return "";
        }
        return header_line.substr(pos, end - pos);
    }

    size_t end = header_line.find(';', pos);
    if (end == string::npos)
    {
        end = header_line.size();
    }
    return trim_copy(header_line.substr(pos, end - pos));
}

string HttpRequest::value(const string &primary, const string &fallback) const
{
    map<string, string>::const_iterator form_it = form.find(primary);
    if (form_it != form.end())
    {
        return form_it->second;
    }

    map<string, string>::const_iterator json_it = json.find(primary);
    if (json_it != json.end())
    {
        return json_it->second;
    }

    if (!fallback.empty())
    {
        form_it = form.find(fallback);
        if (form_it != form.end())
        {
            return form_it->second;
        }

        json_it = json.find(fallback);
        if (json_it != json.end())
        {
            return json_it->second;
        }
    }

    return "";
}

string HttpRequest::query_value(const string &key) const
{
    map<string, string>::const_iterator it = query.find(key);
    return it == query.end() ? "" : it->second;
}

long HttpRequest::query_long_value(const string &key, long fallback, long minimum, long maximum) const
{
    const string raw = trim_copy(query_value(key));
    if (raw.empty())
    {
        return fallback;
    }

    char *endptr = nullptr;
    long value = strtol(raw.c_str(), &endptr, 10);
    if (endptr == raw.c_str() || (endptr != nullptr && *endptr != '\0'))
    {
        return fallback;
    }
    if (value < minimum)
    {
        return minimum;
    }
    if (value > maximum)
    {
        return maximum;
    }
    return value;
}

bool HttpRequest::query_truthy_value(const string &key) const
{
    const string item = query_value(key);
    return item == "1" || item == "true" || item == "TRUE" || item == "yes" || item == "on";
}

string HttpRequest::bearer_token() const
{
    const string authorization = header_value("authorization");
    if (!starts_with_ignore_case(authorization, "Bearer "))
    {
        return "";
    }
    return trim_copy(authorization.substr(7));
}

void HttpResponse::set_body(int status_code, const char *status_title, const string &response_body,
                            const char *response_content_type)
{
    status = status_code;
    title = status_title == nullptr ? "" : status_title;
    content_type = response_content_type == nullptr ? "text/html" : response_content_type;
    body = response_body;
    options_response = false;
    file = FileResponse();
}

void HttpResponse::set_json_error(int status_code, const char *status_title, const string &message)
{
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "{\"code\":%d,\"message\":\"", status_code);
    set_body(status_code, status_title,
             string(prefix) + json_escape(message) + "\"}",
             "application/json");
}

void HttpResponse::set_file(const string &file_path, const string &file_content_type,
                            const string &download_name)
{
    status = 200;
    title = "OK";
    content_type = file_content_type.empty() ? "application/octet-stream" : file_content_type;
    body.clear();
    headers.clear();
    options_response = false;
    file.enabled = true;
    file.path = file_path;
    file.content_type = content_type;
    file.download_name = download_name;
}

void HttpResponse::set_options()
{
    status = 204;
    title = "OK";
    content_type = "text/plain";
    body.clear();
    headers.clear();
    file = FileResponse();
    options_response = true;
}

string make_session_token()
{
    unsigned char token_bytes[kSessionTokenBytes];
    if (RAND_bytes(token_bytes, sizeof(token_bytes)) != 1)
    {
        return "";
    }
    return encode_hex(token_bytes, sizeof(token_bytes));
}
}
