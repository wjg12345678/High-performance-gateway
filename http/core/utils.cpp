#include "connection.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <openssl/evp.h>

using namespace std;

string HttpConnection::url_decode(const string &value) const
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

string HttpConnection::trim_copy(const string &value) const
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

string HttpConnection::json_escape(const string &value) const
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
            escaped.push_back(value[i]);
            break;
        }
    }
    return escaped;
}

bool HttpConnection::decode_base64(const string &input, string &output) const
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

string HttpConnection::header_param_value(const string &header_line, const string &key) const
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

string HttpConnection::request_value(const string &primary, const string &fallback) const
{
    map<string, string>::const_iterator form_it = m_form_data.find(primary);
    if (form_it != m_form_data.end())
    {
        return form_it->second;
    }

    map<string, string>::const_iterator json_it = m_json_data.find(primary);
    if (json_it != m_json_data.end())
    {
        return json_it->second;
    }

    if (!fallback.empty())
    {
        form_it = m_form_data.find(fallback);
        if (form_it != m_form_data.end())
        {
            return form_it->second;
        }

        json_it = m_json_data.find(fallback);
        if (json_it != m_json_data.end())
        {
            return json_it->second;
        }
    }

    return "";
}

string HttpConnection::escape_sql_value(const string &value) const
{
    if (mysql == nullptr)
    {
        return "";
    }

    string escaped(value.size() * 2 + 1, '\0');
    unsigned long escaped_len = mysql_real_escape_string(mysql, &escaped[0], value.c_str(), value.length());
    escaped.resize(escaped_len);
    return escaped;
}

void HttpConnection::set_memory_response(int status, const char *title, const string &body, const char *content_type)
{
    m_response_status = status;
    strncpy(m_response_status_title, title, sizeof(m_response_status_title) - 1);
    strncpy(m_response_content_type, content_type, sizeof(m_response_content_type) - 1);
    m_response_body_storage = body;
    m_response_body = m_response_body_storage.c_str();
    m_response_body_len = m_response_body_storage.size();
}

bool HttpConnection::has_user_session() const
{
    return !m_current_user.empty() && m_current_user != "admin";
}

HttpConnection::HTTP_CODE HttpConnection::respond_json_error(int status, const char *title, const string &message)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"code\":%d,\"message\":\"%s\"}", status, json_escape(message).c_str());
    set_memory_response(status, title, body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::require_user_session(const char *message)
{
    if (has_user_session())
    {
        return NO_REQUEST;
    }
    return respond_json_error(403, "Forbidden", message);
}
