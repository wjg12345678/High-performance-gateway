#include "connection.h"
#include "../api/auth_state.h"

#include <mysql/mysql.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <unistd.h>

#include "../../CGImysql/sql_connection_pool.h"
#include "../../log/log.h"

SSL_CTX *HttpConnection::m_ssl_ctx = nullptr;
bool HttpConnection::m_tls_enabled = false;
bool HttpConnection::m_legacy_compat_enabled = false;
size_t HttpConnection::m_upload_max_bytes = 100 * 1024 * 1024;
size_t HttpConnection::m_upload_request_overhead_bytes = 512 * 1024;

namespace
{
const char *kOk200Title = "OK";
const char *kError400Title = "Bad Request";
const char *kError403Title = "Forbidden";
const char *kError404Title = "Not Found";
const char *kError413Title = "Payload Too Large";
const char *kError501Title = "Not Implemented";
const char *kError500Title = "Internal Error";
const size_t kSessionTokenBytes = 32;

std::string encode_hex(const unsigned char *data, size_t len)
{
    static const char kHexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        hex.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
        hex.push_back(kHexChars[data[i] & 0x0F]);
    }
    return hex;
}
}

void HttpConnection::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = nullptr;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，预加载到内存map
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        auth_user_cache()[temp1] = temp2;
    }
    mysql_free_result(result);
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int HttpConnection::m_user_count = 0;

void HttpConnection::configure_tls(SSL_CTX *ssl_ctx, bool https_enabled)
{
    m_ssl_ctx = ssl_ctx;
    m_tls_enabled = https_enabled && ssl_ctx != nullptr;
}

void HttpConnection::configure_uploads(size_t upload_max_bytes)
{
    if (upload_max_bytes == 0)
    {
        upload_max_bytes = 100 * 1024 * 1024;
    }
    m_upload_max_bytes = upload_max_bytes;
}

void HttpConnection::set_legacy_compat(bool enabled)
{
    m_legacy_compat_enabled = enabled;
}

//关闭连接，关闭一个连接，客户总量减一
void HttpConnection::close_conn(bool real_close)
{
    lock_request();
    close_conn_locked(real_close);
    unlock_request();
}

void HttpConnection::close_conn_locked(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        cleanup_temp_upload_state();
        close_file();
        if (m_ssl != nullptr)
        {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void HttpConnection::init(int epollfd, int sockfd, const sockaddr_in &addr, const string &root, int TRIGMode, int close_log)
{
    lock_request();
    m_epollfd = epollfd;
    m_sockfd = sockfd;
    m_address = addr;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;
    refresh_active();
    m_https_enabled = m_tls_enabled;
    m_tls_handshake_done = !m_https_enabled;
    m_tls_want_event = EPOLLIN;
    if (m_https_enabled)
    {
        m_ssl = SSL_new(m_ssl_ctx);
        if (m_ssl != nullptr)
        {
            SSL_set_fd(m_ssl, m_sockfd);
            SSL_set_accept_state(m_ssl);
            SSL_set_mode(m_ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        }
        else
        {
            m_https_enabled = false;
            m_tls_handshake_done = true;
        }
    }
    else
    {
        m_ssl = nullptr;
    }

    addfd(m_epollfd, sockfd, true, TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;

    init();
    unlock_request();
}

void HttpConnection::refresh_active()
{
    m_last_active = time(nullptr);
}

void HttpConnection::set_request_target(const string &target, const string &query)
{
    m_url_storage = target;
    m_url = m_url_storage.empty() ? nullptr : const_cast<char *>(m_url_storage.c_str());

    m_query_string_storage = query;
    m_query_string = m_query_string_storage.empty() ? nullptr : const_cast<char *>(m_query_string_storage.c_str());
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void HttpConnection::init()
{
    ensure_read_capacity(READ_BUFFER_INITIAL_SIZE);
    ensure_write_capacity(WRITE_BUFFER_INITIAL_SIZE);
    cleanup_temp_upload_state();
    mysql = nullptr;
    m_has_new_data = false;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_header_bytes_sent = 0;
    m_file_offset = 0;
    m_filefd = -1;
    m_response_body = nullptr;
    m_response_body_len = 0;
    m_file_send_offset = 0;
    m_file_send_size = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = nullptr;
    m_query_string = nullptr;
    m_version = nullptr;
    m_content_length = 0;
    m_content_length_seen = false;
    m_host = nullptr;
    m_is_http_1_1 = false;
    m_chunked = false;
    m_chunk_state = CHUNK_STATE_SIZE;
    m_chunked_parse_idx = 0;
    m_chunk_size = 0;
    m_chunk_bytes_read = 0;
    m_chunked_body_bytes_received = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_body_start_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    m_response_status = 200;
    m_request_body.clear();
    m_url_storage.clear();
    m_query_string_storage.clear();
    m_response_body_storage.clear();
    m_extra_headers.clear();
    m_form_data.clear();
    m_json_data.clear();
    m_authorization.clear();
    m_current_user.clear();
    m_body_parse_error_status = 0;
    m_body_parse_error_title.clear();
    m_body_parse_error_message.clear();
    m_stream_body_file = nullptr;
    m_stream_body_tmp_path.clear();
    m_stream_body_bytes_received = 0;
    m_upload_tmp_path.clear();
    m_upload_tmp_filename.clear();
    m_upload_tmp_content_type.clear();
    m_upload_tmp_sha256.clear();
    m_upload_tmp_size = 0;

    memset(m_real_file, '\0', FILENAME_LEN);
    memset(m_content_type, '\0', sizeof(m_content_type));
    memset(m_response_content_type, '\0', sizeof(m_response_content_type));
    memset(m_response_status_title, '\0', sizeof(m_response_status_title));
    std::fill(m_read_ring_buf.begin(), m_read_ring_buf.end(), '\0');
    std::fill(m_read_buf.begin(), m_read_buf.end(), '\0');
    std::fill(m_write_ring_buf.begin(), m_write_ring_buf.end(), '\0');
    std::fill(m_write_buf.begin(), m_write_buf.end(), '\0');
    std::fill(m_file_send_buf.begin(), m_file_send_buf.end(), '\0');
    strncpy(m_response_content_type, "text/html", sizeof(m_response_content_type) - 1);
    strncpy(m_response_status_title, kOk200Title, sizeof(m_response_status_title) - 1);
    reset_ring_buffer(m_read_ring, m_read_ring_buf.data(), (int)m_read_ring_buf.size());
    reset_ring_buffer(m_write_ring, m_write_ring_buf.data(), (int)m_write_ring_buf.size());
}

void HttpConnection::cleanup_temp_upload_state()
{
    if (m_stream_body_file != nullptr)
    {
        fclose(m_stream_body_file);
        m_stream_body_file = nullptr;
    }
    if (!m_stream_body_tmp_path.empty())
    {
        unlink(m_stream_body_tmp_path.c_str());
        m_stream_body_tmp_path.clear();
    }
    if (!m_upload_tmp_path.empty())
    {
        unlink(m_upload_tmp_path.c_str());
        m_upload_tmp_path.clear();
    }
    m_stream_body_bytes_received = 0;
    m_upload_tmp_filename.clear();
    m_upload_tmp_content_type.clear();
    m_upload_tmp_sha256.clear();
    m_upload_tmp_size = 0;
}


//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN

string HttpConnection::make_session_token(const string &username) const
{
    (void)username;

    unsigned char token_bytes[kSessionTokenBytes];
    if (RAND_bytes(token_bytes, sizeof(token_bytes)) != 1)
    {
        return "";
    }
    return encode_hex(token_bytes, sizeof(token_bytes));
}

const char *HttpConnection::method_name() const
{
    switch (m_method)
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

bool HttpConnection::is_api_request() const
{
    return m_url != nullptr && strncasecmp(m_url, "/api/", strlen("/api/")) == 0;
}

bool HttpConnection::requires_auth() const
{
    if (m_url == nullptr)
    {
        return false;
    }
    return strncasecmp(m_url, "/api/private/", strlen("/api/private/")) == 0 ||
           strncasecmp(m_url, "/api/admin/", strlen("/api/admin/")) == 0;
}

bool HttpConnection::should_skip_request_log() const
{
    if (m_url == nullptr)
    {
        return false;
    }

    if (strcasecmp(m_url, "/healthz") == 0)
    {
        return true;
    }

    // Skip high-frequency static GET/HEAD requests to reduce lock contention in request logging.
    if ((m_method == GET || m_method == HEAD) && !is_api_request())
    {
        return true;
    }

    return false;
}


HttpConnection::HTTP_CODE HttpConnection::do_request()
{
    if (m_method == OPTIONS)
    {
        m_response_body = "";
        m_response_body_len = 0;
        return OPTIONS_REQUEST;
    }

    if (!(m_method == GET || m_method == POST || m_method == HEAD || m_method == DELETE))
    {
        return NOT_IMPLEMENTED;
    }

    if (m_method == POST)
    {
        if (!parse_post_body())
        {
            if (m_body_parse_error_status > 0)
            {
                set_memory_response(m_body_parse_error_status,
                                    m_body_parse_error_title.empty() ? kError400Title : m_body_parse_error_title.c_str(),
                                    string("{\"code\":") + std::to_string(m_body_parse_error_status) +
                                        ",\"message\":\"" + json_escape(m_body_parse_error_message.empty() ? "bad request" : m_body_parse_error_message) + "\"}",
                                    "application/json");
                return MEMORY_REQUEST;
            }
            return BAD_REQUEST;
        }
    }

    HTTP_CODE middleware_code = run_before_middlewares();
    if (middleware_code != NO_REQUEST)
    {
        return middleware_code;
    }

    return run_after_middlewares(route_request());
}

HttpConnection::HTTP_CODE HttpConnection::run_before_middlewares()
{
    HTTP_CODE code = middleware_request_log();
    if (code != NO_REQUEST)
    {
        return code;
    }
    return middleware_auth();
}

HttpConnection::HTTP_CODE HttpConnection::run_after_middlewares(HTTP_CODE code)
{
    if (!is_api_request())
    {
        return code;
    }

    if (code == MEMORY_REQUEST || code == FILE_REQUEST || code == OPTIONS_REQUEST)
    {
        return code;
    }

    int status = 500;
    const char *title = kError500Title;
    const char *message = "internal server error";
    if (code == BAD_REQUEST)
    {
        status = 400;
        title = kError400Title;
        message = "bad request";
    }
    else if (code == NO_RESOURCE)
    {
        status = 404;
        title = kError404Title;
        message = "resource not found";
    }
    else if (code == FORBIDDEN_REQUEST)
    {
        status = 403;
        title = kError403Title;
        message = "forbidden";
    }
    else if (code == NOT_IMPLEMENTED)
    {
        status = 501;
        title = kError501Title;
        message = "not implemented";
    }
    else if (code == PAYLOAD_TOO_LARGE)
    {
        status = 413;
        title = kError413Title;
        message = "payload too large";
    }

    char body[128];
    snprintf(body, sizeof(body), "{\"code\":%d,\"message\":\"%s\"}", status, message);
    set_memory_response(status, title, body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::middleware_request_log()
{
    if (0 == m_close_log && !should_skip_request_log())
    {
        Log::get_instance()->write_log(1, "request %s %s content_type=%s content_length=%ld",
                                       method_name(), m_url ? m_url : "",
                                       m_content_type[0] ? m_content_type : "-",
                                       m_content_length);
    }
    return NO_REQUEST;
}
