#include "webserver.h"

WebServer::SubReactor::SubReactor() : m_server(nullptr), m_index(0), m_epollfd(-1), m_notifyfd(-1), m_tid(0), m_stop(false)
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
    return pthread_create(&m_tid, nullptr, worker, this) == 0;
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
        pthread_join(m_tid, nullptr);
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
    SubReactor *reactor = static_cast<SubReactor *>(arg);
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
    m_server->users[connfd].init(m_epollfd, connfd, m_server->m_pending_addresses[connfd],
                                 m_server->m_root, m_server->m_CONNTrigmode, m_server->m_close_log);
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

        if (time(nullptr) - m_server->users[sockfd].last_active() < m_server->m_conn_timeout)
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
