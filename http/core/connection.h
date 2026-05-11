#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <sys/types.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "http_message.h"
#include "ring_buffer.h"
#include "../../infra/lock/locker.h"

typedef struct MYSQL MYSQL;

struct ssl_st;
using SSL = ssl_st;
struct ssl_ctx_st;
using SSL_CTX = ssl_ctx_st;

class connection_pool;

using std::map;
using std::string;
using std::vector;

class HttpConnection
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_INITIAL_SIZE = 16384;
    static const int READ_BUFFER_MAX_SIZE = 2 * 1024 * 1024;
    static const int WRITE_BUFFER_INITIAL_SIZE = 8192;
    using METHOD = http_core::Method;
    static constexpr METHOD GET = http_core::GET;
    static constexpr METHOD POST = http_core::POST;
    static constexpr METHOD HEAD = http_core::HEAD;
    static constexpr METHOD PUT = http_core::PUT;
    static constexpr METHOD DELETE = http_core::DELETE;
    static constexpr METHOD TRACE = http_core::TRACE;
    static constexpr METHOD OPTIONS = http_core::OPTIONS;
    static constexpr METHOD CONNECT = http_core::CONNECT;
    static constexpr METHOD PATH = http_core::PATH;

    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    using HTTP_CODE = http_core::HttpCode;
    static constexpr HTTP_CODE NO_REQUEST = http_core::NO_REQUEST;
    static constexpr HTTP_CODE GET_REQUEST = http_core::GET_REQUEST;
    static constexpr HTTP_CODE BAD_REQUEST = http_core::BAD_REQUEST;
    static constexpr HTTP_CODE NO_RESOURCE = http_core::NO_RESOURCE;
    static constexpr HTTP_CODE FORBIDDEN_REQUEST = http_core::FORBIDDEN_REQUEST;
    static constexpr HTTP_CODE FILE_REQUEST = http_core::FILE_REQUEST;
    static constexpr HTTP_CODE INTERNAL_ERROR = http_core::INTERNAL_ERROR;
    static constexpr HTTP_CODE CLOSED_CONNECTION = http_core::CLOSED_CONNECTION;
    static constexpr HTTP_CODE OPTIONS_REQUEST = http_core::OPTIONS_REQUEST;
    static constexpr HTTP_CODE NOT_IMPLEMENTED = http_core::NOT_IMPLEMENTED;
    static constexpr HTTP_CODE PAYLOAD_TOO_LARGE = http_core::PAYLOAD_TOO_LARGE;
    static constexpr HTTP_CODE MEMORY_REQUEST = http_core::MEMORY_REQUEST;

    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

    enum CHUNK_STATE
    {
        CHUNK_STATE_SIZE = 0,
        CHUNK_STATE_DATA,
        CHUNK_STATE_DATA_CRLF,
        CHUNK_STATE_TRAILER,
        CHUNK_STATE_DONE
    };

public:
    HttpConnection() : mysql(nullptr), m_state(0), timer_flag(0), improv(0), m_epollfd(-1), m_sockfd(-1), m_last_active(0), m_ssl(nullptr), m_https_enabled(false), m_tls_handshake_done(true), m_tls_want_event(EPOLLIN), m_file_send_offset(0), m_file_send_size(0), m_body_parse_error_status(0), m_stream_body_file(nullptr), m_stream_body_bytes_received(0), m_upload_tmp_size(0) {}
    ~HttpConnection() {}
    void lock_request() { m_request_lock.lock(); }
    void unlock_request() { m_request_lock.unlock(); }

public:
    // Public lifecycle and socket state.
    void init(int epollfd, int sockfd, const sockaddr_in &addr, const string &root, int trig_mode, int close_log);
    static void configure_tls(SSL_CTX *ssl_ctx, bool https_enabled);
    static void configure_uploads(size_t upload_max_bytes, size_t user_storage_quota_bytes);
    static void set_legacy_compat(bool enabled);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    int do_tls_handshake();
    void refresh_active();
    time_t last_active() const
    {
        return m_last_active;
    }
    bool is_open() const
    {
        return m_sockfd != -1;
    }
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    bool needs_tls_handshake() const
    {
        return m_https_enabled && !m_tls_handshake_done;
    }
    int timer_flag;
    int improv;

private:
    // Core protocol and connection handling.
    void init();
    bool read_once_et();
    bool flush_streamed_body_from_read_buffer();
    void set_request_target(const string &target, const string &query = "");
    void reset_ring_buffer(ring_buffer &ring, char *storage, int capacity);
    bool ensure_read_capacity(int required_size);
    bool ensure_write_capacity(int required_size);
    int ring_writable(const ring_buffer &ring) const;
    int ring_append(ring_buffer &ring, const char *data, int len);
    int ring_recv(ring_buffer &ring);
    int ring_peek(const ring_buffer &ring, char *dest, int len) const;
    int ring_send(ring_buffer &ring);
    void compact_read_buffer_prefix(long consumed_bytes);
    int socket_send(const char *buffer, int len);
    int wait_event_for_io(int default_event) const;
    bool send_file_over_tls();
    void sync_read_buffer();
    void reset_request_parser_state();
    bool finish_write_cycle();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE parse_chunked_content();
    HTTP_CODE append_decoded_body_chunk(const char *data, long len);
    HTTP_CODE do_request();
    http_core::HttpRequest build_request() const;
    http_core::RequestContext build_request_context(const http_core::HttpRequest &request) const;
    void release_consumed_temp_uploads(const http_core::HttpRequest &request,
                                       const http_core::RequestContext &context);
    HTTP_CODE apply_application_response(const http_core::HttpResponse &response);

    // Request body parsing.
    bool parse_post_body();
    bool parse_form_urlencoded(const string &body);
    bool parse_json_body(const string &body);
    bool parse_multipart_form_data(const string &body);
    bool parse_multipart_form_data_from_file();

    // Shared utility helpers.
    string url_decode(const string &value) const;
    string trim_copy(const string &value) const;
    string json_escape(const string &value) const;
    bool decode_base64(const string &input, string &output) const;
    string header_param_value(const string &header_line, const string &key) const;
    void set_memory_response(int status, const char *title, const string &body, const char *content_type);
    void close_conn_locked(bool real_close = true);

    // Request-body streaming and managed download helpers.
    string make_session_token(const string &username) const;
    bool open_managed_file(const string &path, const string &content_type, const string &download_name);
    bool begin_streamed_body_capture();
    bool append_streamed_body_chunk(const char *data, size_t len);
    void reset_streamed_body_buffer();
    void cleanup_temp_upload_state();

    // Response writing.
    char *get_line() { return m_read_buf.empty() ? nullptr : m_read_buf.data() + m_start_line; };
    LINE_STATUS parse_line();
    void close_file();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length, const char *content_type = "text/html");
    bool add_content_type(const char *content_type);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_keep_alive();
    bool add_allow();
    bool add_blank_line();

public:
    // Shared runtime state.
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_epollfd;
    int m_sockfd;
    sockaddr_in m_address;
    SSL *m_ssl;
    bool m_https_enabled;
    bool m_tls_handshake_done;
    int m_tls_want_event;
    bool m_has_new_data;
    std::vector<char> m_read_ring_buf;
    std::vector<char> m_read_buf;
    long m_read_idx;
    long m_checked_idx;
    long m_body_start_idx;
    int m_start_line;
    std::vector<char> m_write_ring_buf;
    std::vector<char> m_write_buf;
    int m_write_idx;
    ring_buffer m_read_ring;
    ring_buffer m_write_ring;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_query_string;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_content_length_seen;
    bool m_linger;
    bool m_is_http_1_1;
    bool m_chunked;
    CHUNK_STATE m_chunk_state;
    long m_chunked_parse_idx;
    long m_chunk_size;
    long m_chunk_bytes_read;
    long m_chunked_body_bytes_received;
    struct stat m_file_stat;
    int cgi;        //是否启用的POST
    int bytes_to_send;
    int bytes_have_send;
    int m_header_bytes_sent;
    off_t m_file_offset;
    int m_filefd;
    const char *m_response_body;
    int m_response_body_len;
    string doc_root;
    std::vector<char> m_file_send_buf;
    int m_file_send_offset;
    int m_file_send_size;

    int m_TRIGMode;
    int m_close_log;
    time_t m_last_active;
    char m_content_type[256];
    char m_response_content_type[256];
    int m_response_status;
    char m_response_status_title[64];
    string m_request_body;
    string m_url_storage;
    string m_query_string_storage;
    string m_response_body_storage;
    string m_extra_headers;
    map<string, string> m_headers;
    map<string, string> m_form_data;
    map<string, string> m_json_data;
    string m_authorization;
    string m_current_user;
    int m_body_parse_error_status;
    string m_body_parse_error_title;
    string m_body_parse_error_message;
    FILE *m_stream_body_file;
    string m_stream_body_tmp_path;
    long m_stream_body_bytes_received;
    string m_upload_tmp_path;
    string m_upload_tmp_filename;
    string m_upload_tmp_content_type;
    string m_upload_tmp_sha256;
    long m_upload_tmp_size;

    locker m_request_lock;

    static SSL_CTX *m_ssl_ctx;
    static bool m_tls_enabled;
    static bool m_legacy_compat_enabled;
    static size_t m_upload_max_bytes;
    static size_t m_upload_request_overhead_bytes;
    static size_t m_user_storage_quota_bytes;
};

#endif
