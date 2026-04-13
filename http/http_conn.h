#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <map>
#include <time.h>
#include <string>
#include <vector>
#include <mysql/mysql.h>

#include "../lock/locker.h"
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

class connection_pool;

using std::map;
using std::string;
using std::vector;

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_INITIAL_SIZE = 16384;
    static const int READ_BUFFER_MAX_SIZE = 2 * 1024 * 1024;
    static const int WRITE_BUFFER_INITIAL_SIZE = 8192;
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION,
        OPTIONS_REQUEST,
        NOT_IMPLEMENTED,
        MEMORY_REQUEST
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

    typedef HTTP_CODE (http_conn::*RouteHandler)();

public:
    struct ManagedFileRecord
    {
        long file_id;
        long file_size;
        string owner;
        string stored_name;
        string original_name;
        string content_type;

        ManagedFileRecord() : file_id(0), file_size(0) {}
    };

    http_conn() : mysql(NULL), m_state(0), timer_flag(0), improv(0), m_epollfd(-1), m_sockfd(-1), m_last_active(0), m_ssl(NULL), m_https_enabled(false), m_tls_handshake_done(true), m_tls_want_event(EPOLLIN), m_file_send_offset(0), m_file_send_size(0) {}
    ~http_conn() {}
    void lock_request() { m_request_lock.lock(); }
    void unlock_request() { m_request_lock.unlock(); }

public:
    // Public lifecycle and socket state.
    void init(int epollfd, int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    static void configure_tls(SSL_CTX *ssl_ctx, bool https_enabled);
    static void set_auth_token(const string &auth_token);
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
    struct ManagedUploadPayload
    {
        string filename;
        string content;
        string content_type;
    };

    struct ring_buffer
    {
        char *buffer;
        int capacity;
        int head;
        int tail;
        int size;
    };

    // Core protocol and connection handling.
    void init();
    bool read_once_et();
    void reset_ring_buffer(ring_buffer &ring, char *storage, int capacity);
    bool ensure_read_capacity(int required_size);
    bool ensure_write_capacity(int required_size);
    int ring_writable(const ring_buffer &ring) const;
    int ring_append(ring_buffer &ring, const char *data, int len);
    int ring_recv(ring_buffer &ring);
    int ring_peek(const ring_buffer &ring, char *dest, int len) const;
    int ring_send(ring_buffer &ring);
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
    HTTP_CODE do_request();

    // Routing and middleware orchestration.
    HTTP_CODE run_before_middlewares();
    HTTP_CODE run_after_middlewares(HTTP_CODE code);
    HTTP_CODE middleware_request_log();
    HTTP_CODE middleware_auth();
    HTTP_CODE route_api_login();
    HTTP_CODE route_api_register();
    HTTP_CODE route_api_echo();
    HTTP_CODE route_api_private_ping();
    HTTP_CODE route_api_private_logout();
    HTTP_CODE route_healthz();
    HTTP_CODE route_page_login();
    HTTP_CODE route_page_register();
    HTTP_CODE route_register_page();
    HTTP_CODE route_login_page();
    HTTP_CODE route_picture_page();
    HTTP_CODE route_video_page();
    HTTP_CODE route_fans_page();
    HTTP_CODE route_request();
    HTTP_CODE handle_api_request();
    HTTP_CODE handle_static_route();
    HTTP_CODE open_static_file();

    // Request body parsing.
    bool parse_post_body();
    bool parse_form_urlencoded(const string &body);
    bool parse_json_body(const string &body);
    bool parse_multipart_form_data(const string &body);

    // Shared utility helpers.
    string url_decode(const string &value) const;
    string trim_copy(const string &value) const;
    string json_escape(const string &value) const;
    bool decode_base64(const string &input, string &output) const;
    string header_param_value(const string &header_line, const string &key) const;
    string request_value(const string &primary, const string &fallback = "") const;
    string escape_sql_value(const string &value) const;
    void set_memory_response(int status, const char *title, const string &body, const char *content_type);
    bool has_user_session() const;
    HTTP_CODE require_user_session(const char *message);
    HTTP_CODE respond_json_error(int status, const char *title, const string &message);
    const char *method_name() const;
    bool is_api_request() const;
    bool requires_auth() const;

    // Authentication and session management.
    string make_session_token(const string &username) const;
    string make_password_salt() const;
    string hash_password(const string &password, const string &salt) const;
    string extract_bearer_token() const;
    bool lookup_session(const string &token, string &username);
    bool persist_session(const string &token, const string &username, int ttl_seconds);
    bool remove_session(const string &token);
    bool verify_user_password(const string &username, const string &password);
    bool update_user_password_hash(const string &username, const string &password);
    HTTP_CODE handle_auth_request(bool is_register, bool api_mode);
    HTTP_CODE handle_logout_request();

    // File service helpers and handlers.
    string sanitize_filename(const string &value) const;
    string file_storage_root() const;
    string build_file_disk_path(const string &stored_name) const;
    bool ensure_directory(const string &path) const;
    bool fetch_file_record(long file_id, ManagedFileRecord &record);
    HTTP_CODE load_owned_file_record(long file_id, ManagedFileRecord &record);
    bool write_operation_log(const string &username, const string &action, const string &resource_type,
                             long resource_id, const string &detail);
    HTTP_CODE handle_file_upload();
    HTTP_CODE handle_file_list();
    HTTP_CODE handle_file_download(const char *path);
    HTTP_CODE handle_file_delete(const char *path);
    HTTP_CODE handle_operation_list();
    HTTP_CODE handle_operation_delete(const char *path);
    bool open_managed_file(const string &path, const string &content_type, const string &download_name);
    bool parse_managed_upload_payload(ManagedUploadPayload &payload, int &status, const char *&title, string &message);
    string build_file_list_json(MYSQL_RES *result) const;
    string build_operation_list_json(MYSQL_RES *result) const;

    // Static resource resolution and response writing.
    bool resolve_static_path(const char *route_path);
    char *get_line() { return m_read_buf.empty() ? NULL : m_read_buf.data() + m_start_line; };
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
    bool m_linger;
    bool m_is_http_1_1;
    bool m_chunked;
    struct stat m_file_stat;
    int cgi;        //是否启用的POST
    int bytes_to_send;
    int bytes_have_send;
    int m_header_bytes_sent;
    off_t m_file_offset;
    int m_filefd;
    const char *m_response_body;
    int m_response_body_len;
    char *doc_root;
    std::vector<char> m_file_send_buf;
    int m_file_send_offset;
    int m_file_send_size;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;
    time_t m_last_active;
    char m_content_type[256];
    char m_response_content_type[256];
    int m_response_status;
    char m_response_status_title[64];
    string m_request_body;
    string m_response_body_storage;
    string m_extra_headers;
    map<string, string> m_form_data;
    map<string, string> m_json_data;
    string m_authorization;
    string m_current_user;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
    locker m_request_lock;

    static SSL_CTX *m_ssl_ctx;
    static bool m_tls_enabled;
    static string m_auth_token;
};

#endif
