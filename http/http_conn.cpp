#include "http_conn.h"
#include "http_auth_state.h"

#include <mysql/mysql.h>
#include <dirent.h>
#include <errno.h>
#include <fstream>
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <sstream>

#include "../CGImysql/sql_connection_pool.h"
#include "../log/log.h"
#include "../memorypool/memory_pool.h"

SSL_CTX *http_conn::m_ssl_ctx = NULL;
bool http_conn::m_tls_enabled = false;
string http_conn::m_auth_token;

namespace
{
const char *kOk200Title = "OK";
const char *kError400Title = "Bad Request";
const char *kError403Title = "Forbidden";
const char *kError404Title = "Not Found";
const char *kError501Title = "Not Implemented";
const char *kError500Title = "Internal Error";
const size_t kManagedUploadLimitBytes = 64 * 1024;

bool starts_with_ignore_case(const char *text, const char *prefix)
{
    return strncasecmp(text, prefix, strlen(prefix)) == 0;
}

struct RouteEntry
{
    http_conn::METHOD method;
    const char *path;
    http_conn::RouteHandler handler;
};

bool route_matches(const RouteEntry &route, http_conn::METHOD method, const char *url)
{
    return route.method == method && strcasecmp(url, route.path) == 0;
}

const char *detect_static_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
    {
        return "application/octet-stream";
    }

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
    {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(dot, ".css") == 0)
    {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(dot, ".js") == 0)
    {
        return "application/javascript; charset=utf-8";
    }
    if (strcasecmp(dot, ".json") == 0)
    {
        return "application/json; charset=utf-8";
    }
    if (strcasecmp(dot, ".svg") == 0)
    {
        return "image/svg+xml";
    }
    if (strcasecmp(dot, ".png") == 0)
    {
        return "image/png";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
    {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".gif") == 0)
    {
        return "image/gif";
    }
    if (strcasecmp(dot, ".webp") == 0)
    {
        return "image/webp";
    }
    if (strcasecmp(dot, ".mp4") == 0)
    {
        return "video/mp4";
    }
    if (strcasecmp(dot, ".txt") == 0)
    {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}
}

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
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

int http_conn::m_user_count = 0;

void http_conn::configure_tls(SSL_CTX *ssl_ctx, bool https_enabled)
{
    m_ssl_ctx = ssl_ctx;
    m_tls_enabled = https_enabled && ssl_ctx != NULL;
}

void http_conn::set_auth_token(const string &auth_token)
{
    m_auth_token = auth_token;
}

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        close_file();
        if (m_ssl != NULL)
        {
            SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = NULL;
        }
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int epollfd, int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
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
        if (m_ssl != NULL)
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
        m_ssl = NULL;
    }

    addfd(m_epollfd, sockfd, true, TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

void http_conn::refresh_active()
{
    m_last_active = time(NULL);
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    ensure_read_capacity(READ_BUFFER_INITIAL_SIZE);
    ensure_write_capacity(WRITE_BUFFER_INITIAL_SIZE);
    mysql = NULL;
    m_has_new_data = false;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_header_bytes_sent = 0;
    m_file_offset = 0;
    m_filefd = -1;
    m_response_body = NULL;
    m_response_body_len = 0;
    m_file_send_offset = 0;
    m_file_send_size = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_query_string = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_is_http_1_1 = false;
    m_chunked = false;
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
    m_response_body_storage.clear();
    m_extra_headers.clear();
    m_form_data.clear();
    m_json_data.clear();
    m_authorization.clear();
    m_current_user.clear();

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


//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN

string http_conn::make_session_token(const string &username) const
{
    char token[128];
    unsigned int seed = (unsigned int)(time(NULL) ^ m_sockfd ^ (int)username.length());
    snprintf(token, sizeof(token), "%08x%08x%08x",
             (unsigned int)time(NULL),
             (unsigned int)(rand_r(&seed) ^ m_sockfd),
             (unsigned int)(rand_r(&seed) ^ (unsigned int)username.length() << 16));
    return token;
}

const char *http_conn::method_name() const
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

bool http_conn::is_api_request() const
{
    return m_url != NULL && starts_with_ignore_case(m_url, "/api/");
}

bool http_conn::requires_auth() const
{
    if (m_url == NULL)
    {
        return false;
    }
    return starts_with_ignore_case(m_url, "/api/private/") || starts_with_ignore_case(m_url, "/api/admin/");
}


http_conn::HTTP_CODE http_conn::do_request()
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

http_conn::HTTP_CODE http_conn::run_before_middlewares()
{
    HTTP_CODE code = middleware_request_log();
    if (code != NO_REQUEST)
    {
        return code;
    }
    return middleware_auth();
}

http_conn::HTTP_CODE http_conn::run_after_middlewares(HTTP_CODE code)
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

    char body[128];
    snprintf(body, sizeof(body), "{\"code\":%d,\"message\":\"%s\"}", status, message);
    set_memory_response(status, title, body, "application/json");
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::middleware_request_log()
{
    if (0 == m_close_log)
    {
        Log::get_instance()->write_log(1, "request %s %s content_type=%s content_length=%ld",
                                       method_name(), m_url ? m_url : "",
                                       m_content_type[0] ? m_content_type : "-",
                                       m_content_length);
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_request()
{
    static const RouteEntry kRoutes[] = {
        {POST, "/api/login", &http_conn::route_api_login},
        {POST, "/api/register", &http_conn::route_api_register},
        {POST, "/api/echo", &http_conn::route_api_echo},
        {GET, "/api/private/ping", &http_conn::route_api_private_ping},
        {POST, "/api/private/logout", &http_conn::route_api_private_logout},
        {GET, "/healthz", &http_conn::route_healthz},
        {HEAD, "/healthz", &http_conn::route_healthz},
        {POST, "/2", &http_conn::route_page_login},
        {POST, "/2CGISQL.cgi", &http_conn::route_page_login},
        {POST, "/3", &http_conn::route_page_register},
        {POST, "/3CGISQL.cgi", &http_conn::route_page_register},
        {GET, "/0", &http_conn::route_register_page},
        {GET, "/1", &http_conn::route_login_page},
        {GET, "/5", &http_conn::route_picture_page},
        {GET, "/6", &http_conn::route_video_page},
        {GET, "/7", &http_conn::route_fans_page},
        {HEAD, "/0", &http_conn::route_register_page},
        {HEAD, "/1", &http_conn::route_login_page},
        {HEAD, "/5", &http_conn::route_picture_page},
        {HEAD, "/6", &http_conn::route_video_page},
        {HEAD, "/7", &http_conn::route_fans_page},
    };

    for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); ++i)
    {
        if (route_matches(kRoutes[i], m_method, m_url))
        {
            return (this->*kRoutes[i].handler)();
        }
    }

    if (starts_with_ignore_case(m_url, "/api/"))
    {
        return handle_api_request();
    }

    return handle_static_route();
}

http_conn::HTTP_CODE http_conn::route_api_echo()
{
    return handle_api_request();
}

http_conn::HTTP_CODE http_conn::route_api_private_ping()
{
    string body = string("{\"code\":0,\"message\":\"pong\",\"user\":\"") +
                  json_escape(m_current_user) + "\"}";
    set_memory_response(200, kOk200Title,
                        body,
                        "application/json");
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_healthz()
{
    set_memory_response(200, kOk200Title,
                        "{\"code\":0,\"status\":\"ok\"}",
                        "application/json");
    return MEMORY_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_page_login()
{
    return handle_auth_request(false, false);
}

http_conn::HTTP_CODE http_conn::route_page_register()
{
    return handle_auth_request(true, false);
}

http_conn::HTTP_CODE http_conn::route_register_page()
{
    return resolve_static_path("/register.html") ? open_static_file() : BAD_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_login_page()
{
    return resolve_static_path("/log.html") ? open_static_file() : BAD_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_picture_page()
{
    return resolve_static_path("/picture.html") ? open_static_file() : BAD_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_video_page()
{
    return resolve_static_path("/video.html") ? open_static_file() : BAD_REQUEST;
}

http_conn::HTTP_CODE http_conn::route_fans_page()
{
    return resolve_static_path("/fans.html") ? open_static_file() : BAD_REQUEST;
}

http_conn::HTTP_CODE http_conn::handle_static_route()
{
    if (!resolve_static_path(m_url))
    {
        return BAD_REQUEST;
    }

    return open_static_file();
}

http_conn::HTTP_CODE http_conn::open_static_file()
{
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    m_filefd = open(m_real_file, O_RDONLY);
    if (m_filefd < 0)
        return NO_RESOURCE;
    strncpy(m_response_content_type, detect_static_content_type(m_real_file), sizeof(m_response_content_type) - 1);
    return FILE_REQUEST;
}

bool http_conn::resolve_static_path(const char *route_path)
{
    const char *target = route_path;

    if (strcmp(route_path, "/0") == 0)
    {
        target = "/register.html";
    }
    else if (strcmp(route_path, "/1") == 0)
    {
        target = "/log.html";
    }
    else if (strcmp(route_path, "/5") == 0)
    {
        target = "/picture.html";
    }
    else if (strcmp(route_path, "/6") == 0)
    {
        target = "/video.html";
    }
    else if (strcmp(route_path, "/7") == 0)
    {
        target = "/fans.html";
    }

    if (target == NULL || target[0] != '/')
    {
        return false;
    }

    strcpy(m_real_file, doc_root);
    strncpy(m_real_file + strlen(doc_root), target, FILENAME_LEN - (int)strlen(doc_root) - 1);
    m_real_file[FILENAME_LEN - 1] = '\0';
    return true;
}

http_conn::HTTP_CODE http_conn::handle_api_request()
{
    if (strcasecmp(m_url, "/healthz") == 0)
    {
        return route_healthz();
    }

    if (strcasecmp(m_url, "/api/login") == 0)
    {
        return handle_auth_request(false, true);
    }

    if (strcasecmp(m_url, "/api/register") == 0)
    {
        return handle_auth_request(true, true);
    }

    if (strcasecmp(m_url, "/api/echo") == 0)
    {
        string response = "{\"code\":0,\"content_type\":\"" + json_escape(m_content_type) +
                          "\",\"body\":\"" + json_escape(m_request_body) + "\"}";
        set_memory_response(200, kOk200Title, response, "application/json");
        return MEMORY_REQUEST;
    }

    if (strcasecmp(m_url, "/api/private/logout") == 0)
    {
        if (m_method == POST)
        {
            return handle_logout_request();
        }
        return NOT_IMPLEMENTED;
    }

    if (strcasecmp(m_url, "/api/private/files") == 0)
    {
        if (m_method == POST)
        {
            return handle_file_upload();
        }
        if (m_method == GET)
        {
            return handle_file_list();
        }
        return NOT_IMPLEMENTED;
    }

    if (strcasecmp(m_url, "/api/private/operations") == 0)
    {
        if (m_method == GET)
        {
            return handle_operation_list();
        }
        return NOT_IMPLEMENTED;
    }

    if (starts_with_ignore_case(m_url, "/api/private/operations/"))
    {
        const char *suffix = m_url + strlen("/api/private/operations/");
        if (m_method == DELETE)
        {
            return handle_operation_delete(suffix);
        }
    }

    if (starts_with_ignore_case(m_url, "/api/private/files/"))
    {
        const char *suffix = m_url + strlen("/api/private/files/");
        if (m_method == GET && strstr(suffix, "/download") != NULL)
        {
            return handle_file_download(suffix);
        }
        if (m_method == DELETE)
        {
            return handle_file_delete(suffix);
        }
    }

    string response = "{\"code\":404,\"message\":\"api not found\"}";
    set_memory_response(404, kError404Title, response, "application/json");
    return MEMORY_REQUEST;
}
