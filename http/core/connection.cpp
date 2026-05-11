#include "connection.h"
#include "../router/router.h"
#include "../../service/auth/auth_service.h"

#include <mysql/mysql.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <unistd.h>

#include "../../infra/db/sql_connection_pool.h"
#include "../../infra/log/log.h"

SSL_CTX *HttpConnection::m_ssl_ctx = nullptr;
bool HttpConnection::m_tls_enabled = false;
bool HttpConnection::m_legacy_compat_enabled = false;
size_t HttpConnection::m_upload_max_bytes = 100 * 1024 * 1024;
size_t HttpConnection::m_upload_request_overhead_bytes = 512 * 1024;
size_t HttpConnection::m_user_storage_quota_bytes = 1024L * 1024L * 1024L;

namespace
{
const char *kOk200Title = "OK";
const char *kError400Title = "Bad Request";
const char *kError403Title = "Forbidden";
const char *kError404Title = "Not Found";
const char *kError413Title = "Payload Too Large";
const char *kError501Title = "Not Implemented";
const char *kError500Title = "Internal Error";

void parse_query_string(const string &query_string, map<string, string> &query)
{
    size_t start = 0;
    while (start <= query_string.size())
    {
        size_t end = query_string.find('&', start);
        string item = query_string.substr(start, end == string::npos ? string::npos : end - start);
        if (!item.empty())
        {
            size_t eq = item.find('=');
            string key = http_core::url_decode(item.substr(0, eq));
            if (!key.empty())
            {
                query[key] = eq == string::npos ? "" : http_core::url_decode(item.substr(eq + 1));
            }
        }

        if (end == string::npos)
        {
            break;
        }
        start = end + 1;
    }
}

string response_headers_to_wire(const map<string, string> &headers)
{
    string wire;
    for (map<string, string>::const_iterator it = headers.begin(); it != headers.end(); ++it)
    {
        if (!it->first.empty())
        {
            wire += it->first;
            wire += ": ";
            wire += it->second;
            wire += "\r\n";
        }
    }
    return wire;
}
}

void HttpConnection::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = nullptr;
    connectionRAII mysqlcon(&mysql, connPool);

    //在 users 表中检索账号凭据，预加载到内存 map。
    if (!service_auth::load_user_cache(mysql))
    {
        LOG_ERROR("SELECT user cache error:%s\n", mysql_error(mysql));
    }
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

void HttpConnection::configure_uploads(size_t upload_max_bytes, size_t user_storage_quota_bytes)
{
    if (upload_max_bytes == 0)
    {
        upload_max_bytes = 100 * 1024 * 1024;
    }
    m_upload_max_bytes = upload_max_bytes;
    m_user_storage_quota_bytes = user_storage_quota_bytes;
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
    m_headers.clear();
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
    return http_core::make_session_token();
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
                const string message = m_body_parse_error_message.empty() ? "bad request" : m_body_parse_error_message;
                set_memory_response(m_body_parse_error_status,
                                    m_body_parse_error_title.empty() ? kError400Title : m_body_parse_error_title.c_str(),
                                    string("{\"code\":") + std::to_string(m_body_parse_error_status) +
                                        ",\"message\":\"" + json_escape(message) + "\"}",
                                    "application/json");
                return MEMORY_REQUEST;
            }
            return BAD_REQUEST;
        }
    }

    http_core::HttpRequest request = build_request();
    http_core::RequestContext context = build_request_context(request);
    http_core::HttpResponse response;
    HTTP_CODE code = http_router::handle_request(request, context, response);
    release_consumed_temp_uploads(request, context);
    m_current_user = context.current_user;
    if (code == MEMORY_REQUEST || code == FILE_REQUEST || code == OPTIONS_REQUEST)
    {
        return apply_application_response(response);
    }
    return code;
}

http_core::HttpRequest HttpConnection::build_request() const
{
    http_core::HttpRequest request;
    request.method = m_method;
    request.path = m_url == nullptr ? "" : m_url;
    request.query_string = m_query_string == nullptr ? "" : m_query_string;
    request.version = m_version == nullptr ? "" : m_version;
    request.headers = m_headers;
    if (m_host != nullptr && request.headers.find("host") == request.headers.end())
    {
        request.headers["host"] = m_host;
    }
    if (m_content_type[0] != '\0' && request.headers.find("content-type") == request.headers.end())
    {
        request.headers["content-type"] = m_content_type;
    }
    if (!m_authorization.empty() && request.headers.find("authorization") == request.headers.end())
    {
        request.headers["authorization"] = m_authorization;
    }
    parse_query_string(request.query_string, request.query);
    request.content_length = m_content_length;
    request.content_length_seen = m_content_length_seen;
    request.keep_alive = m_linger;
    request.http_1_1 = m_is_http_1_1;
    request.chunked = m_chunked;
    request.body = m_request_body;
    request.form = m_form_data;
    request.json = m_json_data;
    request.upload.temp_path = m_upload_tmp_path;
    request.upload.filename = m_upload_tmp_filename;
    request.upload.content_type = m_upload_tmp_content_type;
    request.upload.sha256 = m_upload_tmp_sha256;
    request.upload.size = m_upload_tmp_size;
    return request;
}

http_core::RequestContext HttpConnection::build_request_context(const http_core::HttpRequest &request) const
{
    http_core::RequestContext context;
    context.mysql = mysql;
    context.doc_root = doc_root;
    context.current_user = m_current_user;
    context.upload_max_bytes = m_upload_max_bytes;
    context.user_storage_quota_bytes = m_user_storage_quota_bytes;
    context.legacy_compat_enabled = m_legacy_compat_enabled;
    context.close_log = m_close_log;
    context.user_agent = request.header_value("user-agent");

    char ip[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &m_address.sin_addr, ip, sizeof(ip)) != nullptr)
    {
        context.client_ip = ip;
    }
    return context;
}

void HttpConnection::release_consumed_temp_uploads(const http_core::HttpRequest &request,
                                                   const http_core::RequestContext &context)
{
    if (request.upload.released && request.upload.temp_path == m_upload_tmp_path)
    {
        m_upload_tmp_path.clear();
        return;
    }

    for (size_t i = 0; i < context.released_temp_upload_paths.size(); ++i)
    {
        if (context.released_temp_upload_paths[i] == m_upload_tmp_path)
        {
            m_upload_tmp_path.clear();
        }
    }
}

HttpConnection::HTTP_CODE HttpConnection::apply_application_response(const http_core::HttpResponse &response)
{
    if (response.options_response)
    {
        m_response_body = "";
        m_response_body_len = 0;
        return OPTIONS_REQUEST;
    }

    if (response.file.enabled)
    {
        if (!open_managed_file(response.file.path, response.file.content_type, response.file.download_name))
        {
            set_memory_response(404, kError404Title,
                                "{\"code\":404,\"message\":\"file content not found\"}",
                                "application/json");
            return MEMORY_REQUEST;
        }
        return FILE_REQUEST;
    }

    set_memory_response(response.status, response.title.c_str(), response.body, response.content_type.c_str());
    m_extra_headers = response_headers_to_wire(response.headers);
    return MEMORY_REQUEST;
}
