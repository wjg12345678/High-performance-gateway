#include "webserver.h"

#include <openssl/err.h>

namespace
{
int set_nonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
}

WebServer::SubReactor::SubReactor() : m_server(NULL), m_index(0), m_epollfd(-1), m_notifyfd(-1), m_tid(0), m_stop(false)
{
}

bool WebServer::SubReactor::init(WebServer *server, int index)
{
    m_server = server;
    m_index = index;

    m_epollfd = epoll_create(5);
    if (m_epollfd == -1)
    {
        return false;
    }

    m_notifyfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_notifyfd == -1)
    {
        close(m_epollfd);
        m_epollfd = -1;
        return false;
    }

    epoll_event event;
    event.data.fd = m_notifyfd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(m_epollfd, EPOLL_CTL_ADD, m_notifyfd, &event);
    return true;
}

bool WebServer::SubReactor::start()
{
    return pthread_create(&m_tid, NULL, worker, this) == 0;
}

void WebServer::SubReactor::stop()
{
    m_stop = true;
    if (m_notifyfd != -1)
    {
        uint64_t one = 1;
        while (write(m_notifyfd, &one, sizeof(one)) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
    }
}

void WebServer::SubReactor::wait()
{
    if (m_tid != 0)
    {
        pthread_join(m_tid, NULL);
        m_tid = 0;
    }
    if (m_notifyfd != -1)
    {
        close(m_notifyfd);
        m_notifyfd = -1;
    }
}

bool WebServer::SubReactor::dispatch(int connfd)
{
    m_pending_lock.lock();
    m_pending_connections.push_back(connfd);
    m_pending_lock.unlock();

    uint64_t one = 1;
    while (true)
    {
        ssize_t ret = write(m_notifyfd, &one, sizeof(one));
        if (ret == (ssize_t)sizeof(one))
        {
            return true;
        }
        if (ret < 0 && errno == EINTR)
        {
            continue;
        }
        m_pending_lock.lock();
        if (!m_pending_connections.empty() && m_pending_connections.back() == connfd)
        {
            m_pending_connections.pop_back();
        }
        m_pending_lock.unlock();
        return false;
    }
}

void *WebServer::SubReactor::worker(void *arg)
{
    SubReactor *reactor = (SubReactor *)arg;
    reactor->run();
    return reactor;
}

void WebServer::SubReactor::run()
{
    epoll_event events[MAX_EVENT_NUMBER];

    while (!m_stop && !g_server_stop && !g_server_reload)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, next_wait_timeout_ms());
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
            if (0 == m_server->m_close_log)
            {
                Log::get_instance()->write_log(3, "sub reactor %d epoll failure", m_index);
                Log::get_instance()->flush();
            }
            continue;
        }

        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == m_notifyfd)
            {
                handle_notify();
                continue;
            }

            if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                m_server->users[sockfd].close_conn();
                remove_connection(sockfd);
                continue;
            }

            if (events[i].events & EPOLLIN)
            {
                m_server->dealwithread(sockfd);
                if (!m_server->users[sockfd].is_open())
                {
                    remove_connection(sockfd);
                }
                else
                {
                    refresh_timer(sockfd);
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                m_server->dealwithwrite(sockfd);
                if (!m_server->users[sockfd].is_open())
                {
                    remove_connection(sockfd);
                }
                else
                {
                    refresh_timer(sockfd);
                }
            }
        }
        scan_timeout();
    }
}

void WebServer::SubReactor::register_connection(int connfd)
{
    m_server->users[connfd].init(m_epollfd, connfd, m_server->m_pending_addresses[connfd], m_server->m_root,
                                         m_server->m_CONNTrigmode, m_server->m_close_log, m_server->m_user,
                                         m_server->m_passWord, m_server->m_databaseName);
    refresh_timer(connfd);
}

void WebServer::SubReactor::handle_notify()
{
    while (true)
    {
        uint64_t ready_count = 0;
        ssize_t ret = read(m_notifyfd, &ready_count, sizeof(ready_count));
        if (ret == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return;
        }
        if (ret == 0)
        {
            break;
        }
        if (ret != (ssize_t)sizeof(ready_count))
        {
            continue;
        }
        if (m_stop)
        {
            break;
        }

        while (true)
        {
            m_pending_lock.lock();
            if (m_pending_connections.empty())
            {
                m_pending_lock.unlock();
                break;
            }
            int connfd = m_pending_connections.front();
            m_pending_connections.pop_front();
            m_pending_lock.unlock();

            if (connfd > 0)
            {
                register_connection(connfd);
            }
        }
    }
}

void WebServer::SubReactor::scan_timeout()
{
    m_timer_heap.tick([this](int sockfd) {
        if (!m_server->users[sockfd].is_open())
        {
            return;
        }

        if (time(NULL) - m_server->users[sockfd].last_active() < m_server->m_conn_timeout)
        {
            m_timer_heap.add_or_update(sockfd, m_server->m_conn_timeout);
            return;
        }

        m_server->users[sockfd].close_conn();
    });
}

void WebServer::SubReactor::refresh_timer(int sockfd)
{
    if (!m_server->users[sockfd].is_open())
    {
        return;
    }
    m_timer_heap.add_or_update(sockfd, m_server->m_conn_timeout);
}

int WebServer::SubReactor::next_wait_timeout_ms()
{
    return m_timer_heap.get_next_timeout_ms();
}

void WebServer::SubReactor::remove_connection(int sockfd)
{
    m_timer_heap.remove(sockfd);
}

WebServer::WebServer() : m_epollfd(-1), m_listenfd(-1), m_pool(NULL), m_https_enable(0), m_ssl_ctx(NULL), m_sub_reactor_num(1), m_next_sub_reactor(0)
{
    users = new http_conn[MAX_FD];
    m_pending_addresses = new sockaddr_in[MAX_FD];

    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);
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
    if (m_ssl_ctx != NULL)
    {
        SSL_CTX_free(m_ssl_ctx);
        m_ssl_ctx = NULL;
    }
    delete[] users;
    delete[] m_pending_addresses;
    delete m_pool;
    free(m_root);
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
        http_conn::configure_tls(NULL, false);
        return true;
    }

    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    m_ssl_ctx = SSL_CTX_new(method);
    if (m_ssl_ctx == NULL)
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

    http_conn::configure_tls(m_ssl_ctx, true);
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
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    m_pool = new threadpool<http_conn>(0, m_connPool, m_thread_num, 10000, m_threadpool_max_threads, m_threadpool_idle_timeout);
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
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
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

        if (http_conn::m_user_count >= MAX_FD)
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
    http_conn::set_auth_token(m_auth_token);
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
        if (!m_pool->append_p(users + sockfd))
        {
            users[sockfd].close_conn();
        }
        return;
    }

    users[sockfd].close_conn();
}

void WebServer::dealwithwrite(int sockfd)
{
    http_conn::set_auth_token(m_auth_token);
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
