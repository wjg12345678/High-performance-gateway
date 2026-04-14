#include "webserver.h"

#include <openssl/err.h>

#include "./CGImysql/sql_connection_pool.h"

WebServer::WebServer()
    : m_https_enable(0), m_ssl_ctx(nullptr), m_epollfd(-1), m_connPool(nullptr),
      m_listenfd(-1), m_sub_reactor_num(1), m_next_sub_reactor(0)
{
    users.resize(MAX_FD);
    m_pending_addresses.resize(MAX_FD);

    char server_path[200];
    getcwd(server_path, 200);
    m_root = std::string(server_path) + "/root";
}

WebServer::~WebServer()
{
    for (size_t i = 0; i < m_sub_reactors.size(); ++i)
    {
        m_sub_reactors[i].stop();
    }
    for (size_t i = 0; i < m_sub_reactors.size(); ++i)
    {
        m_sub_reactors[i].wait();
    }
    if (m_epollfd != -1)
    {
        close(m_epollfd);
    }
    if (m_listenfd != -1)
    {
        close(m_listenfd);
    }
    if (m_ssl_ctx != nullptr)
    {
        SSL_CTX_free(m_ssl_ctx);
        m_ssl_ctx = nullptr;
    }
}

void WebServer::init(int port, string user, string passWord, string databaseName, string dbHost, int dbPort, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int threadpool_max_threads,
                     int threadpool_idle_timeout, int mysql_idle_timeout, int conn_timeout,
                     int close_log, int actor_model, int log_level, int log_split_lines, int log_queue_size,
                     int https_enable, const string &https_cert_file, const string &https_key_file,
                     const string &auth_token)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_dbHost = dbHost;
    m_dbPort = dbPort;
    m_sql_num = sql_num;
    m_mysql_idle_timeout = mysql_idle_timeout;
    m_thread_num = thread_num;
    m_threadpool_max_threads = threadpool_max_threads;
    m_threadpool_idle_timeout = threadpool_idle_timeout;
    m_log_write = log_write;
    m_log_level = log_level;
    m_log_split_lines = log_split_lines;
    m_log_queue_size = log_queue_size;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
    m_https_enable = https_enable;
    m_https_cert_file = https_cert_file;
    m_https_key_file = https_key_file;
    m_auth_token = auth_token;
    m_conn_timeout = conn_timeout > 0 ? conn_timeout : 15;
    m_sub_reactor_num = thread_num > 0 ? thread_num : 1;
    m_next_sub_reactor = 0;
}

bool WebServer::tls_init()
{
    if (!m_https_enable)
    {
        HttpConnection::configure_tls(nullptr, false);
        return true;
    }

    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    m_ssl_ctx = SSL_CTX_new(method);
    if (m_ssl_ctx == nullptr)
    {
        return false;
    }

    SSL_CTX_set_min_proto_version(m_ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(m_ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);

    if (SSL_CTX_use_certificate_file(m_ssl_ctx, m_https_cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
    {
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(m_ssl_ctx, m_https_key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
    {
        return false;
    }

    if (SSL_CTX_check_private_key(m_ssl_ctx) != 1)
    {
        return false;
    }

    HttpConnection::configure_tls(m_ssl_ctx, true);
    return true;
}

void WebServer::trig_mode()
{
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, m_log_split_lines, m_log_queue_size, m_log_level);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, m_log_split_lines, 0, m_log_level);
    }
}

void WebServer::sql_pool()
{
    m_connPool = connection_pool::GetInstance();
    m_connPool->init(m_dbHost, m_user, m_passWord, m_databaseName, m_dbPort, m_sql_num, m_close_log, m_mysql_idle_timeout);
    MYSQL *mysql = nullptr;
    connectionRAII mysqlcon(&mysql, m_connPool);
    if (mysql != nullptr)
    {
        mysql_query(mysql, "ALTER TABLE files ADD COLUMN is_public TINYINT(1) NOT NULL DEFAULT 0");
    }
    users[0].initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    m_pool = std::make_unique<threadpool<HttpConnection>>(0, m_connPool, m_thread_num, 10000,
                                                          m_threadpool_max_threads, m_threadpool_idle_timeout);
}

void WebServer::init_sub_reactors()
{
    m_sub_reactors.resize(m_sub_reactor_num);
    for (int i = 0; i < m_sub_reactor_num; ++i)
    {
        bool ok = m_sub_reactors[i].init(this, i) && m_sub_reactors[i].start();
        assert(ok);
    }
}

void WebServer::eventListen()
{
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(m_conn_timeout);

    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    init_sub_reactors();
}

bool WebServer::dealclientdata()
{
    bool accepted = false;

    while (true)
    {
        struct sockaddr_in client_address;
        socklen_t client_addrlength = sizeof(client_address);
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && 1 == m_LISTENTrigmode)
            {
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
            }
            return accepted;
        }

        if (HttpConnection::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
        }
        else
        {
            SubReactor &reactor = m_sub_reactors[m_next_sub_reactor];
            m_next_sub_reactor = (m_next_sub_reactor + 1) % m_sub_reactor_num;
            m_pending_addresses[connfd] = client_address;
            if (!reactor.dispatch(connfd))
            {
                utils.show_error(connfd, "Dispatch connection failed");
                LOG_ERROR("%s", "dispatch connection failed");
            }
            else
            {
                accepted = true;
            }
        }

        if (0 == m_LISTENTrigmode)
        {
            break;
        }
    }
    return accepted;
}

void WebServer::dealwithread(int sockfd)
{
    HttpConnection::set_auth_token(m_auth_token);
    if (users[sockfd].needs_tls_handshake())
    {
        int handshake_result = users[sockfd].do_tls_handshake();
        if (handshake_result < 0)
        {
            users[sockfd].close_conn();
        }
        return;
    }

    if (users[sockfd].read_once())
    {
        LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        if (!m_pool->append_p(&users[sockfd]))
        {
            users[sockfd].close_conn();
        }
        return;
    }

    users[sockfd].close_conn();
}

void WebServer::dealwithwrite(int sockfd)
{
    HttpConnection::set_auth_token(m_auth_token);
    if (users[sockfd].needs_tls_handshake())
    {
        int handshake_result = users[sockfd].do_tls_handshake();
        if (handshake_result < 0)
        {
            users[sockfd].close_conn();
        }
        return;
    }

    if (users[sockfd].write())
    {
        LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        return;
    }

    users[sockfd].close_conn();
}

void WebServer::eventLoop()
{
    while (!g_server_stop && !g_server_reload)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0)
        {
            if (errno == EINTR)
            {
                if (g_server_stop || g_server_reload)
                {
                    break;
                }
                continue;
            }
            LOG_ERROR("%s", "main reactor epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == m_listenfd)
            {
                dealclientdata();
            }
        }
    }
}
