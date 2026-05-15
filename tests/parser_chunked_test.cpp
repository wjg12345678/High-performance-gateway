#include <cassert>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <mysql/mysql.h>

#include "../http/core/ring_buffer.h"
#include "../http/files/file_types.h"
#include "../infra/lock/locker.h"

#define private public
#include "../http/core/connection.h"
#undef private

#include "../http/core/parser.cpp"

SSL_CTX *HttpConnection::m_ssl_ctx = nullptr;
bool HttpConnection::m_tls_enabled = false;
bool HttpConnection::m_legacy_compat_enabled = false;
size_t HttpConnection::m_upload_max_bytes = 1024;
size_t HttpConnection::m_upload_request_overhead_bytes = 128;

Log::Log() {}
Log::~Log() {}
bool Log::init(const char *, int, int, int, int, int) { return true; }
void Log::shutdown_async() {}
const char *Log::level_name(int) const { return "INFO"; }
void Log::rotate_file(const tm &) {}
std::string Log::build_log_file_path(const tm &, int) const { return ""; }
void Log::reset_queue_locked() {}
void Log::write_log(int, const char *, ...) {}
void Log::flush() {}

void HttpConnection::set_request_target(const std::string &target, const std::string &query)
{
    m_url_storage = target;
    m_url = m_url_storage.empty() ? nullptr : const_cast<char *>(m_url_storage.c_str());
    m_query_string_storage = query;
    m_query_string = m_query_string_storage.empty() ? nullptr : const_cast<char *>(m_query_string_storage.c_str());
}

void HttpConnection::reset_ring_buffer(ring_buffer &ring, char *storage, int capacity)
{
    ring.buffer = storage;
    ring.capacity = capacity;
    ring.head = 0;
    ring.tail = 0;
    ring.size = 0;
}

int HttpConnection::ring_writable(const ring_buffer &ring) const
{
    return ring.capacity - ring.size;
}

int HttpConnection::ring_append(ring_buffer &ring, const char *data, int len)
{
    if (len > ring_writable(ring))
    {
        return -1;
    }
    int first = ring.capacity - ring.tail;
    if (first > len)
    {
        first = len;
    }
    memcpy(ring.buffer + ring.tail, data, first);
    memcpy(ring.buffer, data + first, len - first);
    ring.tail = (ring.tail + len) % ring.capacity;
    ring.size += len;
    return len;
}

int HttpConnection::ring_peek(const ring_buffer &ring, char *dest, int len) const
{
    if (len > ring.size)
    {
        return -1;
    }
    int first = ring.capacity - ring.head;
    if (first > len)
    {
        first = len;
    }
    memcpy(dest, ring.buffer + ring.head, first);
    memcpy(dest + first, ring.buffer, len - first);
    return len;
}

void HttpConnection::sync_read_buffer()
{
    if ((int)m_read_buf.size() < m_read_ring.size + 1)
    {
        m_read_buf.assign(m_read_ring.size + 1, '\0');
    }
    ring_peek(m_read_ring, m_read_buf.data(), m_read_ring.size);
    m_read_idx = m_read_ring.size;
    m_read_buf[m_read_idx] = '\0';
}

void HttpConnection::compact_read_buffer_prefix(long consumed_bytes)
{
    if (consumed_bytes <= 0)
    {
        return;
    }
    if (consumed_bytes > m_read_ring.size)
    {
        consumed_bytes = m_read_ring.size;
    }

    sync_read_buffer();
    const long remaining = m_read_idx - consumed_bytes;
    if (remaining > 0)
    {
        memmove(m_read_buf.data(), m_read_buf.data() + consumed_bytes, static_cast<size_t>(remaining));
    }
    m_read_buf[remaining] = '\0';
    reset_ring_buffer(m_read_ring, m_read_ring_buf.data(), (int)m_read_ring_buf.size());
    if (remaining > 0)
    {
        ring_append(m_read_ring, m_read_buf.data(), static_cast<int>(remaining));
    }
    m_read_idx = remaining;
    m_checked_idx = 0;
    m_body_start_idx = 0;
    m_start_line = 0;
}

bool HttpConnection::begin_streamed_body_capture()
{
    m_stream_body_file = reinterpret_cast<FILE *>(1);
    m_stream_body_tmp_path = "parser-test-stream";
    m_stream_body_bytes_received = 0;
    return true;
}

bool HttpConnection::append_streamed_body_chunk(const char *data, size_t len)
{
    m_request_body.append(data, len);
    m_stream_body_bytes_received += static_cast<long>(len);
    return true;
}

void HttpConnection::reset_streamed_body_buffer()
{
    compact_read_buffer_prefix(m_read_idx);
    m_check_state = CHECK_STATE_CONTENT;
}

std::string HttpConnection::trim_copy(const std::string &value) const
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

std::string HttpConnection::url_decode(const std::string &value) const { return value; }
std::string HttpConnection::header_param_value(const std::string &, const std::string &) const { return ""; }
HttpConnection::HTTP_CODE HttpConnection::do_request() { return MEMORY_REQUEST; }
bool HttpConnection::parse_multipart_form_data_from_file() { return true; }

namespace http_file_helpers
{
std::string sanitize_filename(const std::string &filename) { return filename; }
}

namespace http_core
{
std::string trim_copy(const std::string &value)
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

std::string lowercase_copy(const std::string &value)
{
    std::string lowered;
    lowered.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        lowered.push_back(static_cast<char>(tolower(static_cast<unsigned char>(value[i]))));
    }
    return lowered;
}
}

static void reset_parser(HttpConnection &conn)
{
    conn.m_close_log = 1;
    conn.m_read_ring_buf.assign(HttpConnection::READ_BUFFER_INITIAL_SIZE, '\0');
    conn.m_read_buf.assign(HttpConnection::READ_BUFFER_INITIAL_SIZE + 1, '\0');
    conn.reset_ring_buffer(conn.m_read_ring, conn.m_read_ring_buf.data(), (int)conn.m_read_ring_buf.size());
    conn.m_read_idx = 0;
    conn.m_checked_idx = 0;
    conn.m_body_start_idx = 0;
    conn.m_start_line = 0;
    conn.m_check_state = HttpConnection::CHECK_STATE_REQUESTLINE;
    conn.m_method = HttpConnection::GET;
    conn.m_url = nullptr;
    conn.m_query_string = nullptr;
    conn.m_version = nullptr;
    conn.m_host = nullptr;
    conn.m_content_length = 0;
    conn.m_content_length_seen = false;
    conn.m_linger = false;
    conn.m_is_http_1_1 = false;
    conn.m_chunked = false;
    conn.m_chunk_state = HttpConnection::CHUNK_STATE_SIZE;
    conn.m_chunked_parse_idx = 0;
    conn.m_chunk_size = 0;
    conn.m_chunk_bytes_read = 0;
    conn.m_chunked_body_bytes_received = 0;
    conn.m_request_body.clear();
    conn.m_url_storage.clear();
    conn.m_query_string_storage.clear();
    conn.m_stream_body_file = nullptr;
    conn.m_stream_body_tmp_path.clear();
    conn.m_stream_body_bytes_received = 0;
    memset(conn.m_content_type, 0, sizeof(conn.m_content_type));
}

static void feed(HttpConnection &conn, const std::string &data)
{
    assert(conn.ring_append(conn.m_read_ring, data.data(), (int)data.size()) == (int)data.size());
    conn.sync_read_buffer();
}

static void parses_split_chunked_body()
{
    HttpConnection conn;
    reset_parser(conn);
    feed(conn, "POST /api/echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n\r\n4\r\nWi");
    assert(conn.process_read() == HttpConnection::NO_REQUEST);
    assert(conn.m_chunked);
    assert(conn.m_request_body == "Wi");

    feed(conn, "ki\r\n5;ext=value\r\npedia\r\n0\r\n\r\n");
    assert(conn.process_read() == HttpConnection::MEMORY_REQUEST);
    assert(conn.m_request_body == "Wikipedia");
}

static void rejects_invalid_chunk_size()
{
    HttpConnection conn;
    reset_parser(conn);
    feed(conn, "POST /api/echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nnope\r\n0\r\n\r\n");
    assert(conn.process_read() == HttpConnection::BAD_REQUEST);
}

static void rejects_chunked_with_content_length()
{
    HttpConnection conn;
    reset_parser(conn);
    feed(conn, "POST /api/echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    assert(conn.process_read() == HttpConnection::BAD_REQUEST);
}

static void streams_multipart_chunked_body()
{
    HttpConnection conn;
    reset_parser(conn);
    feed(conn, "POST /api/drive/files/upload HTTP/1.1\r\nHost: localhost\r\nContent-Type: multipart/form-data; boundary=x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    assert(conn.process_read() == HttpConnection::MEMORY_REQUEST);
    assert(conn.m_stream_body_file != nullptr);
    assert(conn.m_stream_body_bytes_received == 5);
    assert(conn.m_request_body == "abcde");
}

int main()
{
    parses_split_chunked_body();
    rejects_invalid_chunk_size();
    rejects_chunked_with_content_length();
    streams_multipart_chunked_body();
    return 0;
}
