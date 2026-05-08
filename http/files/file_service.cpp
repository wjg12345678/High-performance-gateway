#include "../core/connection.h"
#include "file_helpers.h"
#include "file_store.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <openssl/evp.h>
#include <fstream>
#include <map>
#include <openssl/sha.h>
#include <unistd.h>

using namespace std;

namespace
{
const int kDefaultListLimit = 20;
const int kMaxListLimit = 100;
const size_t kMultipartTextFieldLimitBytes = 64 * 1024;
const size_t kStoredNamePrefixLength = 24;
const size_t kFilenameMaxLength = 80;

bool ends_with_ignore_case(const string &value, const string &suffix)
{
    if (suffix.size() > value.size())
    {
        return false;
    }

    const size_t offset = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        if (tolower(static_cast<unsigned char>(value[offset + i])) !=
            tolower(static_cast<unsigned char>(suffix[i])))
        {
            return false;
        }
    }
    return true;
}

bool starts_with_html_document(const string &path)
{
    ifstream input(path.c_str(), ios::in | ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    char buffer[256];
    input.read(buffer, sizeof(buffer));
    const streamsize count = input.gcount();
    if (count <= 0)
    {
        return false;
    }

    string sample(buffer, static_cast<size_t>(count));
    size_t pos = 0;
    while (pos < sample.size())
    {
        unsigned char ch = static_cast<unsigned char>(sample[pos]);
        if (ch == 0xEF && pos + 2 < sample.size() &&
            static_cast<unsigned char>(sample[pos + 1]) == 0xBB &&
            static_cast<unsigned char>(sample[pos + 2]) == 0xBF)
        {
            pos += 3;
            continue;
        }
        if (!isspace(ch))
        {
            break;
        }
        ++pos;
    }

    const string prefix = sample.substr(pos);
    return prefix.compare(0, 15, "<!DOCTYPE html") == 0 ||
           prefix.compare(0, 5, "<html") == 0 ||
           prefix.compare(0, 14, "<!doctype html") == 0;
}

void normalize_download_metadata(const string &path, string &content_type, string &download_name)
{
    if (ends_with_ignore_case(download_name, ".html") || ends_with_ignore_case(download_name, ".htm"))
    {
        if (content_type.empty() || content_type == "text/plain" || content_type == "application/octet-stream")
        {
            content_type = "text/html; charset=utf-8";
        }
        return;
    }

    if ((content_type == "text/plain" || content_type == "application/octet-stream") &&
        starts_with_html_document(path))
    {
        content_type = "text/html; charset=utf-8";
        download_name += ".html";
    }
}

bool parse_public_flag(const string &value)
{
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
}

bool file_exists_at_path(const string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

string trim_ascii_copy(const string &value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start])))
    {
        ++start;
    }
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(start, end - start);
}

string lowercase_ascii_copy(const string &value)
{
    string lowered = value;
    for (size_t i = 0; i < lowered.size(); ++i)
    {
        lowered[i] = static_cast<char>(tolower(static_cast<unsigned char>(lowered[i])));
    }
    return lowered;
}

string encode_sha256_hex(const unsigned char *data, size_t len)
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

string duplicate_filename_candidate(const string &name, int index)
{
    if (index <= 0)
    {
        return name;
    }

    const string ext = http_file_helpers::file_extension(name);
    string stem = name.substr(0, name.size() - ext.size());
    const string suffix = " (" + to_string(index) + ")";
    if (stem.size() + suffix.size() + ext.size() > kFilenameMaxLength)
    {
        const size_t max_stem = kFilenameMaxLength - suffix.size() - ext.size();
        stem = stem.substr(0, max_stem);
    }
    return stem + suffix + ext;
}

bool move_file_or_copy(const string &source, const string &target)
{
    if (rename(source.c_str(), target.c_str()) == 0)
    {
        return true;
    }

    if (errno != EXDEV)
    {
        return false;
    }

    ifstream input(source.c_str(), ios::in | ios::binary);
    ofstream output(target.c_str(), ios::out | ios::binary | ios::trunc);
    if (!input.is_open() || !output.is_open())
    {
        return false;
    }

    char buffer[65536];
    while (input.good())
    {
        input.read(buffer, sizeof(buffer));
        streamsize count = input.gcount();
        if (count > 0)
        {
            output.write(buffer, count);
        }
    }
    output.close();
    input.close();

    if (!output.good())
    {
        unlink(target.c_str());
        return false;
    }

    unlink(source.c_str());
    return true;
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

    const string &value() const
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
    string m_value;
};

class MultipartFileSink
{
public:
    MultipartFileSink(const string &path, size_t max_bytes)
        : m_path(path), m_output(path.c_str(), ios::out | ios::binary | ios::trunc),
          m_ctx(EVP_MD_CTX_new()), m_size(0), m_max_bytes(max_bytes), m_limit_exceeded(false), m_finished(false)
    {
        if (m_ctx != nullptr)
        {
            EVP_DigestInit_ex(m_ctx, EVP_sha256(), nullptr);
        }
    }

    ~MultipartFileSink()
    {
        if (m_ctx != nullptr)
        {
            EVP_MD_CTX_free(m_ctx);
            m_ctx = nullptr;
        }
    }

    bool is_open() const
    {
        return m_output.is_open() && m_ctx != nullptr;
    }

    bool write(const char *data, size_t len)
    {
        if (!m_output.is_open())
        {
            return false;
        }
        if (m_size + static_cast<long>(len) > static_cast<long>(m_max_bytes))
        {
            m_limit_exceeded = true;
            return false;
        }
        if (len == 0)
        {
            return true;
        }

        m_output.write(data, len);
        if (!m_output.good())
        {
            return false;
        }
        if (EVP_DigestUpdate(m_ctx, data, len) != 1)
        {
            return false;
        }
        m_size += static_cast<long>(len);
        return true;
    }

    bool finish()
    {
        if (m_finished)
        {
            return true;
        }
        m_output.flush();
        if (!m_output.good())
        {
            return false;
        }
        m_output.close();

        unsigned char digest[SHA256_DIGEST_LENGTH];
        unsigned int digest_len = 0;
        if (EVP_DigestFinal_ex(m_ctx, digest, &digest_len) != 1)
        {
            return false;
        }
        if (digest_len != SHA256_DIGEST_LENGTH)
        {
            return false;
        }
        m_sha256 = encode_sha256_hex(digest, sizeof(digest));
        m_finished = true;
        return true;
    }

    long size() const
    {
        return m_size;
    }

    const string &sha256() const
    {
        return m_sha256;
    }

    bool limit_exceeded() const
    {
        return m_limit_exceeded;
    }

private:
    string m_path;
    ofstream m_output;
    EVP_MD_CTX *m_ctx;
    long m_size;
    size_t m_max_bytes;
    bool m_limit_exceeded;
    bool m_finished;
    string m_sha256;
};

class MultipartTempFileReader
{
public:
    explicit MultipartTempFileReader(const string &path) : m_file(fopen(path.c_str(), "rb")), m_eof(false) {}

    ~MultipartTempFileReader()
    {
        if (m_file != nullptr)
        {
            fclose(m_file);
            m_file = nullptr;
        }
    }

    bool is_open() const
    {
        return m_file != nullptr;
    }

    bool read_line(string &line)
    {
        while (true)
        {
            const size_t pos = m_buffer.find("\r\n");
            if (pos != string::npos)
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

    bool read_headers(map<string, string> &headers)
    {
        headers.clear();
        string line;
        while (read_line(line))
        {
            if (line.empty())
            {
                return true;
            }

            const size_t colon = line.find(':');
            if (colon == string::npos)
            {
                return false;
            }

            headers[lowercase_ascii_copy(trim_ascii_copy(line.substr(0, colon)))] =
                trim_ascii_copy(line.substr(colon + 1));
        }
        return false;
    }

    template <typename Sink>
    bool read_part_body(const string &boundary, Sink &sink, bool &is_final)
    {
        const string delimiter = "\r\n--" + boundary;
        auto consume_delimiter = [&]() -> int
        {
            const size_t pos = m_buffer.find(delimiter);
            if (pos == string::npos)
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
        };

        while (true)
        {
            int delimiter_status = consume_delimiter();
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

            delimiter_status = consume_delimiter();
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
        const size_t read_count = fread(chunk, 1, sizeof(chunk), m_file);
        if (read_count > 0)
        {
            m_buffer.append(chunk, read_count);
            return true;
        }

        if (ferror(m_file))
        {
            return false;
        }

        m_eof = true;
        return !m_buffer.empty();
    }

private:
    FILE *m_file;
    bool m_eof;
    string m_buffer;
};
}

bool HttpConnection::begin_streamed_body_capture()
{
    if (m_stream_body_file != nullptr)
    {
        return true;
    }

    const string storage_root = http_file_helpers::file_storage_root(doc_root);
    const string temp_root = http_file_helpers::temp_storage_root(doc_root);
    if (!http_file_helpers::ensure_directory(storage_root) ||
        !http_file_helpers::ensure_directory(temp_root))
    {
        return false;
    }

    const string token = make_session_token("multipart-body");
    if (token.empty())
    {
        return false;
    }

    m_stream_body_tmp_path = temp_root + "/body-" + token + ".tmp";
    m_stream_body_file = fopen(m_stream_body_tmp_path.c_str(), "wb");
    if (m_stream_body_file == nullptr)
    {
        m_stream_body_tmp_path.clear();
        return false;
    }

    m_stream_body_bytes_received = 0;
    m_request_body.clear();
    return true;
}

bool HttpConnection::append_streamed_body_chunk(const char *data, size_t len)
{
    if (m_stream_body_file == nullptr)
    {
        return false;
    }

    if (m_stream_body_bytes_received + static_cast<long>(len) > m_content_length)
    {
        m_body_parse_error_status = 400;
        m_body_parse_error_title = "Bad Request";
        m_body_parse_error_message = "request body overflow";
        return false;
    }

    if (len == 0)
    {
        return true;
    }

    const size_t written = fwrite(data, 1, len, m_stream_body_file);
    if (written != len)
    {
        m_body_parse_error_status = 500;
        m_body_parse_error_title = "Internal Error";
        m_body_parse_error_message = "failed to spool multipart body";
        return false;
    }

    m_stream_body_bytes_received += static_cast<long>(len);
    return true;
}

void HttpConnection::reset_streamed_body_buffer()
{
    reset_ring_buffer(m_read_ring, m_read_ring_buf.data(), (int)m_read_ring_buf.size());
    m_read_idx = 0;
    m_checked_idx = 0;
    m_body_start_idx = 0;
    m_start_line = 0;
    m_check_state = CHECK_STATE_CONTENT;
    if (!m_read_buf.empty())
    {
        m_read_buf[0] = '\0';
    }
}

bool HttpConnection::parse_multipart_form_data_from_file()
{
    if (m_stream_body_file != nullptr)
    {
        fclose(m_stream_body_file);
        m_stream_body_file = nullptr;
    }

    if (m_stream_body_tmp_path.empty())
    {
        m_body_parse_error_status = 400;
        m_body_parse_error_title = "Bad Request";
        m_body_parse_error_message = "missing multipart body";
        return false;
    }

    string content_type = m_content_type;
    const size_t boundary_pos = content_type.find("boundary=");
    if (boundary_pos == string::npos)
    {
        m_body_parse_error_status = 400;
        m_body_parse_error_title = "Bad Request";
        m_body_parse_error_message = "missing multipart boundary";
        return false;
    }

    string boundary = trim_copy(content_type.substr(boundary_pos + strlen("boundary=")));
    if (!boundary.empty() && boundary[0] == '"' && boundary[boundary.size() - 1] == '"')
    {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    if (boundary.empty())
    {
        m_body_parse_error_status = 400;
        m_body_parse_error_title = "Bad Request";
        m_body_parse_error_message = "invalid multipart boundary";
        return false;
    }

    MultipartTempFileReader reader(m_stream_body_tmp_path);
    if (!reader.is_open())
    {
        m_body_parse_error_status = 500;
        m_body_parse_error_title = "Internal Error";
        m_body_parse_error_message = "failed to open multipart spool file";
        return false;
    }

    string line;
    if (!reader.read_line(line) || line != "--" + boundary)
    {
        m_body_parse_error_status = 400;
        m_body_parse_error_title = "Bad Request";
        m_body_parse_error_message = "invalid multipart preamble";
        return false;
    }

    bool found_file_part = false;
    bool is_final = false;
    while (!is_final)
    {
        map<string, string> headers;
        if (!reader.read_headers(headers))
        {
            m_body_parse_error_status = 400;
            m_body_parse_error_title = "Bad Request";
            m_body_parse_error_message = "invalid multipart headers";
            return false;
        }

        const map<string, string>::const_iterator disposition_it = headers.find("content-disposition");
        if (disposition_it == headers.end())
        {
            m_body_parse_error_status = 400;
            m_body_parse_error_title = "Bad Request";
            m_body_parse_error_message = "missing content-disposition";
            return false;
        }

        const string field_name = header_param_value(disposition_it->second, "name");
        const string field_filename = header_param_value(disposition_it->second, "filename");
        const map<string, string>::const_iterator type_it = headers.find("content-type");
        const string field_content_type = type_it != headers.end() ? type_it->second : "";

        if (!field_filename.empty())
        {
            if (found_file_part)
            {
                m_body_parse_error_status = 400;
                m_body_parse_error_title = "Bad Request";
                m_body_parse_error_message = "multiple file parts are not supported";
                return false;
            }

            const string safe_filename = http_file_helpers::sanitize_filename(field_filename);
            const string temp_token = make_session_token(safe_filename);
            if (temp_token.empty())
            {
                m_body_parse_error_status = 500;
                m_body_parse_error_title = "Internal Error";
                m_body_parse_error_message = "failed to allocate upload temp path";
                return false;
            }

            const string temp_path = http_file_helpers::temp_storage_root(doc_root) +
                                     "/upload-" + temp_token + http_file_helpers::file_extension(safe_filename);
            MultipartFileSink sink(temp_path, m_upload_max_bytes);
            if (!sink.is_open())
            {
                m_body_parse_error_status = 500;
                m_body_parse_error_title = "Internal Error";
                m_body_parse_error_message = "failed to create upload temp file";
                return false;
            }

            if (!reader.read_part_body(boundary, sink, is_final))
            {
                unlink(temp_path.c_str());
                m_body_parse_error_status = sink.limit_exceeded() ? 413 : 400;
                m_body_parse_error_title = sink.limit_exceeded() ? "Payload Too Large" : "Bad Request";
                m_body_parse_error_message = sink.limit_exceeded()
                                                 ? string("upload exceeds limit of ") + to_string(m_upload_max_bytes) + " bytes"
                                                 : "invalid multipart file content";
                return false;
            }

            if (sink.size() <= 0)
            {
                unlink(temp_path.c_str());
                m_body_parse_error_status = 400;
                m_body_parse_error_title = "Bad Request";
                m_body_parse_error_message = "content is empty";
                return false;
            }

            m_upload_tmp_path = temp_path;
            m_upload_tmp_filename = safe_filename;
            m_upload_tmp_content_type = field_content_type.empty() ? "application/octet-stream" : field_content_type;
            m_upload_tmp_sha256 = sink.sha256();
            m_upload_tmp_size = sink.size();
            found_file_part = true;

            if (!field_name.empty())
            {
                m_form_data[field_name] = safe_filename;
            }
        }
        else
        {
            MultipartTextSink sink(kMultipartTextFieldLimitBytes);
            if (!reader.read_part_body(boundary, sink, is_final))
            {
                m_body_parse_error_status = sink.limit_exceeded() ? 413 : 400;
                m_body_parse_error_title = sink.limit_exceeded() ? "Payload Too Large" : "Bad Request";
                m_body_parse_error_message = sink.limit_exceeded()
                                                 ? "multipart form field is too large"
                                                 : "invalid multipart field value";
                return false;
            }

            if (!field_name.empty())
            {
                m_form_data[field_name] = sink.value();
            }
        }
    }

    unlink(m_stream_body_tmp_path.c_str());
    m_stream_body_tmp_path.clear();

    if (!found_file_part)
    {
        m_body_parse_error_status = 400;
        m_body_parse_error_title = "Bad Request";
        m_body_parse_error_message = "missing file field";
        return false;
    }

    return true;
}

HttpConnection::HTTP_CODE HttpConnection::load_owned_file_record(long file_id, ManagedFileRecord &record, bool include_deleted)
{
    if (!http_file_store::fetch_file_record(mysql, file_id, record, include_deleted))
    {
        return respond_json_error(404, "Not Found", "file not found");
    }

    if (record.owner != m_current_user)
    {
        return respond_json_error(403, "Forbidden", "forbidden");
    }

    return NO_REQUEST;
}

string HttpConnection::build_file_list_json(MYSQL_RES *result, long next_cursor, int limit, bool include_deleted) const
{
    string items;
    MYSQL_ROW row;
    bool first = true;
    int count = 0;

    while (count < limit && (row = mysql_fetch_row(result)) != nullptr)
    {
        if (!first)
        {
            items += ",";
        }
        first = false;
        ++count;

        const string stored_name = row[7] ? row[7] : "";
        const string deleted_at = row[9] ? row[9] : "";
        const bool is_deleted = !deleted_at.empty();
        const bool content_available = !stored_name.empty() &&
                                       file_exists_at_path(http_file_helpers::build_file_disk_path(doc_root, stored_name));

        items += "{\"id\":";
        items += row[0] ? row[0] : "0";
        items += ",\"filename\":\"";
        items += json_escape(row[1] ? row[1] : "");
        items += "\",\"content_type\":\"";
        items += json_escape(row[2] ? row[2] : "");
        items += "\",\"size\":";
        items += row[3] ? row[3] : "0";
        items += ",\"is_public\":";
        items += row[5] ? row[5] : "0";
        items += ",\"owner\":\"";
        items += json_escape(row[6] ? row[6] : "");
        items += "\",\"sha256\":\"";
        items += json_escape(row[8] ? row[8] : "");
        items += "\",\"created_at\":\"";
        items += json_escape(row[4] ? row[4] : "");
        items += "\",\"is_deleted\":";
        items += is_deleted ? "true" : "false";
        items += ",\"content_available\":";
        items += content_available ? "true" : "false";
        items += ",\"deleted_at\":";
        if (deleted_at.empty())
        {
            items += "null";
        }
        else
        {
            items += "\"";
            items += json_escape(deleted_at);
            items += "\"";
        }
        items += "}";
    }

    string body = "{\"code\":0,\"files\":[";
    body += items;
    body += "],\"pagination\":{\"limit\":";
    body += to_string(limit);
    body += ",\"next_cursor\":";
    body += to_string(next_cursor);
    body += ",\"has_more\":";
    body += next_cursor > 0 ? "true" : "false";
    body += "},\"view\":\"";
    body += include_deleted ? "trash" : "active";
    body += "\"}";
    return body;
}

string HttpConnection::ensure_unique_owned_filename(const string &requested_name, long ignore_file_id)
{
    string sanitized = http_file_helpers::sanitize_filename(requested_name);
    if (sanitized.empty())
    {
        sanitized = "file.txt";
    }

    for (int index = 0; index < 1000; ++index)
    {
        const string candidate = duplicate_filename_candidate(sanitized, index);
        const string ignore_clause = ignore_file_id > 0 ? string(" AND id<>") + to_string(ignore_file_id) : "";
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "SELECT id FROM files WHERE owner_username='%s' AND original_name='%s' AND deleted_at IS NULL%s LIMIT 1",
                 escape_sql_value(m_current_user).c_str(),
                 escape_sql_value(candidate).c_str(),
                 ignore_clause.c_str());

        if (mysql_query(mysql, sql) != 0)
        {
            return candidate;
        }

        MYSQL_RES *result = mysql_store_result(mysql);
        if (result == nullptr)
        {
            return candidate;
        }

        const bool exists = mysql_fetch_row(result) != nullptr;
        mysql_free_result(result);
        if (!exists)
        {
            return candidate;
        }
    }

    return duplicate_filename_candidate(sanitized, 1000);
}

bool HttpConnection::parse_managed_upload_payload(ManagedUploadPayload &payload, int &status, const char *&title, string &message)
{
    payload.filename = http_file_helpers::sanitize_filename(request_value("filename", "name"));
    payload.content_type = request_value("content_type", "file_content_type");

    if (!m_upload_tmp_path.empty())
    {
        if (payload.filename.empty())
        {
            payload.filename = http_file_helpers::sanitize_filename(m_upload_tmp_filename);
        }
        if (payload.filename.empty())
        {
            payload.filename = "file.bin";
        }
        if (payload.content_type.empty())
        {
            payload.content_type = m_upload_tmp_content_type.empty() ? "application/octet-stream" : m_upload_tmp_content_type;
        }

        payload.temp_path = m_upload_tmp_path;
        payload.sha256 = m_upload_tmp_sha256;
        payload.size = m_upload_tmp_size;
        payload.use_temp_file = true;
        return true;
    }

    if (!m_legacy_compat_enabled)
    {
        status = 400;
        title = "Bad Request";
        message = "multipart/form-data file upload required";
        return false;
    }

    payload.content = request_value("content", "file");
    if (payload.content.empty())
    {
        const string content_base64 = request_value("content_base64", "file_base64");
        if (!content_base64.empty() && !decode_base64(content_base64, payload.content))
        {
            status = 400;
            title = "Bad Request";
            message = "invalid base64 content";
            return false;
        }
    }

    if (payload.content.empty())
    {
        status = 400;
        title = "Bad Request";
        message = "content is empty";
        return false;
    }

    if (payload.filename.empty())
    {
        payload.filename = "file.txt";
    }
    if (payload.content_type.empty())
    {
        payload.content_type = "text/plain";
    }
    if (payload.content.size() > m_upload_max_bytes)
    {
        status = 413;
        title = "Payload Too Large";
        message = string("upload exceeds limit of ") + to_string(m_upload_max_bytes) + " bytes";
        return false;
    }

    payload.size = payload.content.size();
    return true;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_upload()
{
    HTTP_CODE auth_code = require_user_session("file upload requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    ManagedUploadPayload payload;
    int error_status = 200;
    const char *error_title = "OK";
    string error_message;
    if (!parse_managed_upload_payload(payload, error_status, error_title, error_message))
    {
        return respond_json_error(error_status, error_title, error_message);
    }

    if (!http_file_helpers::ensure_directory(http_file_helpers::file_storage_root(doc_root)))
    {
        return INTERNAL_ERROR;
    }

    payload.filename = ensure_unique_owned_filename(payload.filename);
    const bool is_public = parse_public_flag(request_value("is_public"));
    if (payload.sha256.empty() && !payload.content.empty())
    {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char *>(payload.content.data()), payload.content.size(), digest);
        payload.sha256 = encode_sha256_hex(digest, sizeof(digest));
    }

    const string stored_name_prefix = make_session_token(payload.filename);
    if (stored_name_prefix.empty())
    {
        return INTERNAL_ERROR;
    }

    string stored_name = stored_name_prefix.substr(0, kStoredNamePrefixLength);
    if (!payload.sha256.empty())
    {
        stored_name += "_" + payload.sha256.substr(0, 12);
    }
    stored_name += http_file_helpers::file_extension(payload.filename);
    const string disk_path = http_file_helpers::build_file_disk_path(doc_root, stored_name);

    if (payload.use_temp_file)
    {
        if (!move_file_or_copy(payload.temp_path, disk_path))
        {
            return INTERNAL_ERROR;
        }
        if (payload.temp_path == m_upload_tmp_path)
        {
            m_upload_tmp_path.clear();
        }
    }
    else
    {
        ofstream output(disk_path.c_str(), ios::out | ios::binary | ios::trunc);
        if (!output.is_open())
        {
            return INTERNAL_ERROR;
        }
        output.write(payload.content.data(), payload.content.size());
        output.close();
        if (!output.good())
        {
            unlink(disk_path.c_str());
            return INTERNAL_ERROR;
        }
    }

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "INSERT INTO files(owner_username, stored_name, original_name, content_type, file_size, is_public, content_sha256) "
             "VALUES('%s', '%s', '%s', '%s', %ld, %d, '%s')",
             escape_sql_value(m_current_user).c_str(),
             escape_sql_value(stored_name).c_str(),
             escape_sql_value(payload.filename).c_str(),
             escape_sql_value(payload.content_type).c_str(),
             payload.size,
             is_public ? 1 : 0,
             escape_sql_value(payload.sha256).c_str());
    if (mysql_query(mysql, sql) != 0)
    {
        unlink(disk_path.c_str());
        return INTERNAL_ERROR;
    }

    const long file_id = static_cast<long>(mysql_insert_id(mysql));
    write_operation_log(m_current_user, "upload", "file", file_id, payload.filename);

    string body = "{\"code\":0,\"message\":\"upload success\",\"file\":{\"id\":";
    body += to_string(file_id);
    body += ",\"filename\":\"";
    body += json_escape(payload.filename);
    body += "\",\"size\":";
    body += to_string(payload.size);
    body += ",\"is_public\":";
    body += is_public ? "true" : "false";
    body += ",\"sha256\":\"";
    body += json_escape(payload.sha256);
    body += "\"}}";
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_list()
{
    HTTP_CODE auth_code = require_user_session("file list requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    const long limit = query_long_value("limit", kDefaultListLimit, 1, kMaxListLimit);
    const long cursor = query_long_value("cursor", 0, 0, 2147483647L);
    const bool include_deleted = query_truthy_value("include_deleted") || query_truthy_value("trash");

    string sql = "SELECT id, original_name, content_type, file_size, "
                 "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), is_public, owner_username, stored_name, "
                 "COALESCE(content_sha256, ''), COALESCE(DATE_FORMAT(deleted_at, '%Y-%m-%d %H:%i:%s'), '') "
                 "FROM files WHERE owner_username='" + escape_sql_value(m_current_user) + "'";
    sql += include_deleted ? " AND deleted_at IS NOT NULL" : " AND deleted_at IS NULL";
    if (cursor > 0)
    {
        sql += " AND id<" + to_string(cursor);
    }
    sql += " ORDER BY id DESC LIMIT " + to_string(limit + 1);

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return INTERNAL_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return INTERNAL_ERROR;
    }

    long next_cursor = 0;
    if (mysql_num_rows(result) > static_cast<my_ulonglong>(limit))
    {
        mysql_data_seek(result, static_cast<my_ulonglong>(limit));
        MYSQL_ROW row = mysql_fetch_row(result);
        next_cursor = (row != nullptr && row[0] != nullptr) ? atol(row[0]) : 0;
        mysql_data_seek(result, 0);
    }

    const string body = build_file_list_json(result, next_cursor, static_cast<int>(limit), include_deleted);
    mysql_free_result(result);
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_public_file_list()
{
    const long limit = query_long_value("limit", kDefaultListLimit, 1, kMaxListLimit);
    const long cursor = query_long_value("cursor", 0, 0, 2147483647L);

    string sql = "SELECT id, original_name, content_type, file_size, "
                 "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), is_public, owner_username, stored_name, "
                 "COALESCE(content_sha256, ''), '' "
                 "FROM files WHERE is_public=1 AND deleted_at IS NULL";
    if (cursor > 0)
    {
        sql += " AND id<" + to_string(cursor);
    }
    sql += " ORDER BY id DESC LIMIT " + to_string(limit + 1);

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return INTERNAL_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return INTERNAL_ERROR;
    }

    long next_cursor = 0;
    if (mysql_num_rows(result) > static_cast<my_ulonglong>(limit))
    {
        mysql_data_seek(result, static_cast<my_ulonglong>(limit));
        MYSQL_ROW row = mysql_fetch_row(result);
        next_cursor = (row != nullptr && row[0] != nullptr) ? atol(row[0]) : 0;
        mysql_data_seek(result, 0);
    }

    const string body = build_file_list_json(result, next_cursor, static_cast<int>(limit), false);
    mysql_free_result(result);
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_public_file_detail(const char *path)
{
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    if (!http_file_store::fetch_file_record(mysql, file_id, record))
    {
        return respond_json_error(404, "Not Found", "file not found");
    }
    if (!record.is_public)
    {
        return respond_json_error(403, "Forbidden", "file is private");
    }

    const string disk_path = http_file_helpers::build_file_disk_path(doc_root, record.stored_name);
    if (!file_exists_at_path(disk_path))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    struct stat st;
    const long actual_size = stat(disk_path.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : record.file_size;

    string body = "{\"code\":0,\"file\":{\"id\":";
    body += to_string(record.file_id);
    body += ",\"filename\":\"";
    body += json_escape(record.original_name);
    body += "\",\"content_type\":\"";
    body += json_escape(record.content_type);
    body += "\",\"size\":";
    body += to_string(actual_size);
    body += ",\"owner\":\"";
    body += json_escape(record.owner);
    body += "\",\"is_public\":true,\"sha256\":\"";
    body += json_escape(record.content_sha256);
    body += "\",\"download_url\":\"/api/files/public/";
    body += to_string(record.file_id);
    body += "/download\"}}";
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_download(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file download requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    HTTP_CODE record_code = load_owned_file_record(file_id, record);
    if (record_code != NO_REQUEST)
    {
        return record_code;
    }

    if (!open_managed_file(http_file_helpers::build_file_disk_path(doc_root, record.stored_name),
                           record.content_type, record.original_name))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    write_operation_log(m_current_user, "download", "file", file_id, record.original_name);
    return FILE_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_public_file_download(const char *path)
{
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    if (!http_file_store::fetch_file_record(mysql, file_id, record))
    {
        return respond_json_error(404, "Not Found", "file not found");
    }
    if (!record.is_public)
    {
        return respond_json_error(403, "Forbidden", "file is private");
    }

    if (!open_managed_file(http_file_helpers::build_file_disk_path(doc_root, record.stored_name),
                           record.content_type, record.original_name))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    write_operation_log(m_current_user.empty() ? "guest" : m_current_user,
                        "public_download", "file", file_id, record.original_name);
    return FILE_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_delete(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file delete requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    HTTP_CODE record_code = load_owned_file_record(file_id, record);
    if (record_code != NO_REQUEST)
    {
        return record_code;
    }

    char sql[512];
    snprintf(sql, sizeof(sql),
             "UPDATE files SET deleted_at=NOW(), is_public=0 WHERE id=%ld AND deleted_at IS NULL",
             file_id);
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    write_operation_log(m_current_user, "delete", "file", file_id, record.original_name);
    set_memory_response(200, "OK",
                        "{\"code\":0,\"message\":\"file moved to recycle bin\"}",
                        "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_visibility_update(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file visibility update requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/visibility") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    HTTP_CODE record_code = load_owned_file_record(file_id, record);
    if (record_code != NO_REQUEST)
    {
        return record_code;
    }

    const bool is_public = parse_public_flag(request_value("is_public"));
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE files SET is_public=%d WHERE id=%ld", is_public ? 1 : 0, file_id);
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    write_operation_log(m_current_user, is_public ? "publish" : "unpublish", "file", file_id, record.original_name);
    set_memory_response(200, "OK",
                        is_public ? "{\"code\":0,\"message\":\"file is now public\"}"
                                  : "{\"code\":0,\"message\":\"file is now private\"}",
                        "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_file_restore(const char *path)
{
    HTTP_CODE auth_code = require_user_session("file restore requires user session");
    if (auth_code != NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/restore") != 0)
    {
        return BAD_REQUEST;
    }

    ManagedFileRecord record;
    HTTP_CODE record_code = load_owned_file_record(file_id, record, true);
    if (record_code != NO_REQUEST)
    {
        return record_code;
    }
    if (!record.is_deleted)
    {
        return respond_json_error(409, "Conflict", "file is not in recycle bin");
    }

    if (!file_exists_at_path(http_file_helpers::build_file_disk_path(doc_root, record.stored_name)))
    {
        return respond_json_error(404, "Not Found", "file content not found");
    }

    const string restored_name = ensure_unique_owned_filename(record.original_name, file_id);
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "UPDATE files SET deleted_at=NULL, is_public=0, original_name='%s' WHERE id=%ld",
             escape_sql_value(restored_name).c_str(),
             file_id);
    if (mysql_query(mysql, sql) != 0)
    {
        return INTERNAL_ERROR;
    }

    write_operation_log(m_current_user, "restore", "file", file_id, restored_name);
    string body = "{\"code\":0,\"message\":\"file restored\",\"file\":{\"id\":";
    body += to_string(file_id);
    body += ",\"filename\":\"";
    body += json_escape(restored_name);
    body += "\"}}";
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

bool HttpConnection::open_managed_file(const string &path, const string &content_type, const string &download_name)
{
    strncpy(m_real_file, path.c_str(), sizeof(m_real_file) - 1);
    m_real_file[sizeof(m_real_file) - 1] = '\0';

    if (stat(m_real_file, &m_file_stat) < 0 || S_ISDIR(m_file_stat.st_mode))
    {
        return false;
    }

    m_filefd = open(m_real_file, O_RDONLY);
    if (m_filefd < 0)
    {
        return false;
    }

    string resolved_content_type = content_type.empty() ? "application/octet-stream" : content_type;
    string resolved_download_name = download_name.empty() ? "download.txt" : download_name;
    normalize_download_metadata(path, resolved_content_type, resolved_download_name);

    strncpy(m_response_content_type, resolved_content_type.c_str(), sizeof(m_response_content_type) - 1);
    string safe_name = http_file_helpers::sanitize_download_filename(resolved_download_name);
    string encoded_name = http_file_helpers::encode_download_filename(resolved_download_name);
    m_extra_headers = string("Content-Disposition: attachment; filename=\"") + safe_name +
                      "\"; filename*=UTF-8''" + encoded_name + "\r\n";
    return true;
}
