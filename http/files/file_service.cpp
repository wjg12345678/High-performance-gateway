#include "../core/connection.h"
#include "file_helpers.h"
#include "file_store.h"
#include "multipart_parser.h"
#include "../../infra/storage/storage.h"
#include "../../repo/mysql/file_repository.h"
#include "../../service/files/file_service.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sys/stat.h>

using namespace std;

namespace
{
const int kDefaultListLimit = 20;
const int kMaxListLimit = 100;
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

bool has_filename_extension(const string &name)
{
    const size_t slash = name.find_last_of("/\\");
    const size_t dot = name.find_last_of('.');
    return dot != string::npos && (slash == string::npos || dot > slash + 1) && dot + 1 < name.size();
}

string lower_ascii_copy(const string &value)
{
    string lowered;
    lowered.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        lowered.push_back(static_cast<char>(tolower(static_cast<unsigned char>(value[i]))));
    }
    return lowered;
}

string content_type_extension(const string &content_type)
{
    const string lowered = lower_ascii_copy(content_type);
    const size_t semicolon = lowered.find(';');
    const string type = semicolon == string::npos ? lowered : lowered.substr(0, semicolon);
    if (type == "image/jpeg" || type == "image/jpg") return ".jpg";
    if (type == "image/png") return ".png";
    if (type == "image/gif") return ".gif";
    if (type == "image/webp") return ".webp";
    if (type == "image/bmp") return ".bmp";
    if (type == "image/svg+xml") return ".svg";
    if (type == "video/mp4") return ".mp4";
    if (type == "audio/mpeg") return ".mp3";
    if (type == "application/pdf") return ".pdf";
    if (type == "application/zip") return ".zip";
    if (type == "application/json") return ".json";
    if (type == "text/plain") return ".txt";
    if (type == "text/html") return ".html";
    return "";
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
    if (download_name.empty())
    {
        download_name = "download";
    }

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
        if (!has_filename_extension(download_name))
        {
            download_name += ".html";
        }
        return;
    }

    if (!has_filename_extension(download_name))
    {
        download_name += content_type_extension(content_type);
    }
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

bool parse_non_negative_long(const string &value, long &number)
{
    const string trimmed = trim_ascii_copy(value);
    if (trimmed.empty())
    {
        return false;
    }

    char *endptr = nullptr;
    number = strtol(trimmed.c_str(), &endptr, 10);
    return endptr != trimmed.c_str() && endptr != nullptr && *endptr == '\0' && number >= 0;
}

bool is_valid_share_token(const string &token)
{
    if (token.empty() || token.size() > 64)
    {
        return false;
    }
    for (size_t i = 0; i < token.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(token[i]);
        if (!isalnum(ch) && ch != '-' && ch != '_')
        {
            return false;
        }
    }
    return true;
}

bool parse_share_path(const char *path, bool download, string &token)
{
    const string suffix = path == nullptr ? string() : string(path);
    const string download_suffix = "/download";
    if (download)
    {
        if (suffix.size() <= download_suffix.size() ||
            suffix.compare(suffix.size() - download_suffix.size(), download_suffix.size(), download_suffix) != 0)
        {
            return false;
        }
        token = suffix.substr(0, suffix.size() - download_suffix.size());
    }
    else
    {
        if (suffix.find('/') != string::npos)
        {
            return false;
        }
        token = suffix;
    }
    return is_valid_share_token(token);
}

}

bool HttpConnection::begin_streamed_body_capture()
{
    if (m_stream_body_file != nullptr)
    {
        return true;
    }

    const string storage_root = infra_storage::storage_root(doc_root);
    const string temp_root = infra_storage::temp_root(doc_root);
    if (!infra_storage::ensure_directory(storage_root) ||
        !infra_storage::ensure_directory(temp_root))
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

    const long body_limit = m_chunked
                                ? static_cast<long>(m_upload_max_bytes + m_upload_request_overhead_bytes)
                                : m_content_length;
    if (m_stream_body_bytes_received + static_cast<long>(len) > body_limit)
    {
        m_body_parse_error_status = m_chunked ? 413 : 400;
        m_body_parse_error_title = m_chunked ? "Payload Too Large" : "Bad Request";
        m_body_parse_error_message = m_chunked
                                         ? string("upload exceeds limit of ") + to_string(m_upload_max_bytes) + " bytes"
                                         : "request body overflow";
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

    http_multipart::ParseResult result;
    http_multipart::ParseError error;
    if (!http_multipart::parse_spooled_multipart(m_stream_body_tmp_path, m_content_type, doc_root,
                                                 m_upload_max_bytes, result, error))
    {
        m_body_parse_error_status = error.status;
        m_body_parse_error_title = error.title;
        m_body_parse_error_message = error.message;
        return false;
    }

    m_form_data.insert(result.fields.begin(), result.fields.end());
    m_upload_tmp_path = result.file.temp_path;
    m_upload_tmp_filename = result.file.filename;
    m_upload_tmp_content_type = result.file.content_type;
    m_upload_tmp_sha256 = result.file.sha256;
    m_upload_tmp_size = result.file.size;

    infra_storage::remove_file(m_stream_body_tmp_path);
    m_stream_body_tmp_path.clear();
    return true;
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
    string resolved_download_name = download_name.empty() ? "download" : download_name;
    normalize_download_metadata(path, resolved_content_type, resolved_download_name);

    strncpy(m_response_content_type, resolved_content_type.c_str(), sizeof(m_response_content_type) - 1);
    string safe_name = http_file_helpers::sanitize_download_filename(resolved_download_name);
    string encoded_name = http_file_helpers::encode_download_filename(resolved_download_name);
    m_extra_headers = string("Content-Disposition: attachment; filename=\"") + safe_name +
                      "\"; filename*=UTF-8''" + encoded_name + "\r\n";
    return true;
}
