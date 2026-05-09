#include "multipart_parser.h"

#include "file_helpers.h"
#include "../../infra/storage/storage.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <openssl/rand.h>

namespace
{
const size_t kMultipartTextFieldLimitBytes = 64 * 1024;

std::string trim_ascii_copy(const std::string &value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
    {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(start, end - start);
}

std::string lowercase_ascii_copy(const std::string &value)
{
    std::string lowered = value;
    for (size_t i = 0; i < lowered.size(); ++i)
    {
        lowered[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lowered[i])));
    }
    return lowered;
}

std::string header_param_value(const std::string &header_line, const std::string &key)
{
    std::string pattern = key + "=";
    size_t pos = header_line.find(pattern);
    if (pos == std::string::npos)
    {
        return "";
    }

    pos += pattern.size();
    while (pos < header_line.size() && std::isspace(static_cast<unsigned char>(header_line[pos])))
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
        if (end == std::string::npos)
        {
            return "";
        }
        return header_line.substr(pos, end - pos);
    }

    size_t end = header_line.find(';', pos);
    if (end == std::string::npos)
    {
        end = header_line.size();
    }
    return trim_ascii_copy(header_line.substr(pos, end - pos));
}

std::string random_hex_token()
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    {
        return "";
    }

    static const char kHexChars[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(bytes) * 2);
    for (size_t i = 0; i < sizeof(bytes); ++i)
    {
        token.push_back(kHexChars[(bytes[i] >> 4) & 0x0F]);
        token.push_back(kHexChars[bytes[i] & 0x0F]);
    }
    return token;
}

class MultipartTextSink
{
public:
    explicit MultipartTextSink(size_t limit) : m_limit(limit), m_limit_exceeded(false) {}

    bool write(const char *data, size_t len)
    {
        if (m_value.size() + len > m_limit)
        {
            m_limit_exceeded = true;
            return false;
        }
        if (len > 0)
        {
            m_value.append(data, len);
        }
        return true;
    }

    bool finish()
    {
        return true;
    }

    const std::string &value() const
    {
        return m_value;
    }

    bool limit_exceeded() const
    {
        return m_limit_exceeded;
    }

private:
    size_t m_limit;
    bool m_limit_exceeded;
    std::string m_value;
};

class MultipartTempFileReader
{
public:
    explicit MultipartTempFileReader(const std::string &path) : m_file(std::fopen(path.c_str(), "rb")), m_eof(false) {}

    ~MultipartTempFileReader()
    {
        if (m_file != nullptr)
        {
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

    bool is_open() const
    {
        return m_file != nullptr;
    }

    bool read_line(std::string &line)
    {
        while (true)
        {
            const size_t pos = m_buffer.find("\r\n");
            if (pos != std::string::npos)
            {
                line = m_buffer.substr(0, pos);
                m_buffer.erase(0, pos + 2);
                return true;
            }

            if (!fill())
            {
                return false;
            }
        }
    }

    bool read_headers(std::map<std::string, std::string> &headers)
    {
        headers.clear();
        std::string line;
        while (read_line(line))
        {
            if (line.empty())
            {
                return true;
            }

            const size_t colon = line.find(':');
            if (colon == std::string::npos)
            {
                return false;
            }

            headers[lowercase_ascii_copy(trim_ascii_copy(line.substr(0, colon)))] =
                trim_ascii_copy(line.substr(colon + 1));
        }
        return false;
    }

    template <typename Sink>
    bool read_part_body(const std::string &boundary, Sink &sink, bool &is_final)
    {
        const std::string delimiter = "\r\n--" + boundary;
        while (true)
        {
            int delimiter_status = consume_delimiter(delimiter, sink, is_final);
            if (delimiter_status >= 0)
            {
                return delimiter_status == 1;
            }

            if (m_eof)
            {
                return false;
            }

            if (!fill() && m_eof)
            {
                return false;
            }

            delimiter_status = consume_delimiter(delimiter, sink, is_final);
            if (delimiter_status >= 0)
            {
                return delimiter_status == 1;
            }

            if (m_buffer.size() > delimiter.size())
            {
                const size_t flush = m_buffer.size() - delimiter.size();
                if (!sink.write(m_buffer.data(), flush))
                {
                    return false;
                }
                m_buffer.erase(0, flush);
            }
        }
    }

private:
    bool fill()
    {
        if (m_file == nullptr || m_eof)
        {
            return false;
        }

        char chunk[8192];
        const size_t read_count = std::fread(chunk, 1, sizeof(chunk), m_file);
        if (read_count > 0)
        {
            m_buffer.append(chunk, read_count);
            return true;
        }

        if (std::ferror(m_file))
        {
            return false;
        }

        m_eof = true;
        return !m_buffer.empty();
    }

    template <typename Sink>
    int consume_delimiter(const std::string &delimiter, Sink &sink, bool &is_final)
    {
        const size_t pos = m_buffer.find(delimiter);
        if (pos == std::string::npos)
        {
            return -1;
        }

        if (pos > 0 && !sink.write(m_buffer.data(), pos))
        {
            return 0;
        }

        m_buffer.erase(0, pos + delimiter.size());
        while (m_buffer.size() < 2 && !m_eof)
        {
            fill();
        }

        if (m_buffer.size() < 2)
        {
            return 0;
        }

        if (m_buffer.compare(0, 2, "--") == 0)
        {
            m_buffer.erase(0, 2);
            is_final = true;
            if (m_buffer.size() < 2 && !m_eof)
            {
                fill();
            }
            if (m_buffer.size() >= 2 && m_buffer.compare(0, 2, "\r\n") == 0)
            {
                m_buffer.erase(0, 2);
            }
            return sink.finish() ? 1 : 0;
        }

        if (m_buffer.compare(0, 2, "\r\n") == 0)
        {
            m_buffer.erase(0, 2);
            is_final = false;
            return sink.finish() ? 1 : 0;
        }

        return 0;
    }

private:
    FILE *m_file;
    bool m_eof;
    std::string m_buffer;
};
}

namespace http_multipart
{
bool parse_spooled_multipart(const std::string &spool_path, const std::string &content_type,
                             const std::string &doc_root, size_t upload_max_bytes,
                             ParseResult &result, ParseError &error)
{
    result = ParseResult();

    auto fail = [&](int status, const std::string &title, const std::string &message) -> bool
    {
        if (!result.file.temp_path.empty())
        {
            infra_storage::remove_file(result.file.temp_path);
        }
        result = ParseResult();
        error.status = status;
        error.title = title;
        error.message = message;
        return false;
    };

    if (spool_path.empty())
    {
        return fail(400, "Bad Request", "missing multipart body");
    }

    const size_t boundary_pos = content_type.find("boundary=");
    if (boundary_pos == std::string::npos)
    {
        return fail(400, "Bad Request", "missing multipart boundary");
    }

    std::string boundary = trim_ascii_copy(content_type.substr(boundary_pos + std::strlen("boundary=")));
    if (!boundary.empty() && boundary[0] == '"' && boundary[boundary.size() - 1] == '"')
    {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    if (boundary.empty())
    {
        return fail(400, "Bad Request", "invalid multipart boundary");
    }

    MultipartTempFileReader reader(spool_path);
    if (!reader.is_open())
    {
        return fail(500, "Internal Error", "failed to open multipart spool file");
    }

    std::string line;
    if (!reader.read_line(line) || line != "--" + boundary)
    {
        return fail(400, "Bad Request", "invalid multipart preamble");
    }

    bool is_final = false;
    while (!is_final)
    {
        std::map<std::string, std::string> headers;
        if (!reader.read_headers(headers))
        {
            return fail(400, "Bad Request", "invalid multipart headers");
        }

        const std::map<std::string, std::string>::const_iterator disposition_it = headers.find("content-disposition");
        if (disposition_it == headers.end())
        {
            return fail(400, "Bad Request", "missing content-disposition");
        }

        const std::string field_name = header_param_value(disposition_it->second, "name");
        const std::string field_filename = header_param_value(disposition_it->second, "filename");
        const std::map<std::string, std::string>::const_iterator type_it = headers.find("content-type");
        const std::string field_content_type = type_it != headers.end() ? type_it->second : "";

        if (!field_filename.empty())
        {
            if (result.has_file)
            {
                return fail(400, "Bad Request", "multiple file parts are not supported");
            }

            const std::string safe_filename = http_file_helpers::sanitize_filename(field_filename);
            const std::string temp_token = random_hex_token();
            if (temp_token.empty())
            {
                return fail(500, "Internal Error", "failed to allocate upload temp path");
            }

            const std::string temp_path = infra_storage::temp_root(doc_root) +
                                          "/upload-" + temp_token + http_file_helpers::file_extension(safe_filename);
            infra_storage::FileSha256Sink sink(temp_path, upload_max_bytes);
            if (!sink.is_open())
            {
                return fail(500, "Internal Error", "failed to create upload temp file");
            }

            if (!reader.read_part_body(boundary, sink, is_final))
            {
                infra_storage::remove_file(temp_path);
                return fail(sink.limit_exceeded() ? 413 : 400,
                            sink.limit_exceeded() ? "Payload Too Large" : "Bad Request",
                            sink.limit_exceeded()
                                ? std::string("upload exceeds limit of ") + std::to_string(upload_max_bytes) + " bytes"
                                : "invalid multipart file content");
            }

            if (sink.size() <= 0)
            {
                infra_storage::remove_file(temp_path);
                return fail(400, "Bad Request", "content is empty");
            }

            result.file.temp_path = temp_path;
            result.file.filename = safe_filename;
            result.file.content_type = field_content_type.empty() ? "application/octet-stream" : field_content_type;
            result.file.sha256 = sink.sha256();
            result.file.size = sink.size();
            result.has_file = true;

            if (!field_name.empty())
            {
                result.fields[field_name] = safe_filename;
            }
        }
        else
        {
            MultipartTextSink sink(kMultipartTextFieldLimitBytes);
            if (!reader.read_part_body(boundary, sink, is_final))
            {
                return fail(sink.limit_exceeded() ? 413 : 400,
                            sink.limit_exceeded() ? "Payload Too Large" : "Bad Request",
                            sink.limit_exceeded() ? "multipart form field is too large" : "invalid multipart field value");
            }

            if (!field_name.empty())
            {
                result.fields[field_name] = sink.value();
            }
        }
    }

    if (!result.has_file)
    {
        return fail(400, "Bad Request", "missing file field");
    }

    return true;
}
}
