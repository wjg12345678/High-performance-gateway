#include "connection.h"

#include <errno.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "../../log/log.h"

using namespace std;

void modfd(int epollfd, int fd, int ev, int TRIGMode);

namespace
{
constexpr int ET_READ_EVENT_MAX_RECVS = 64;
}

void HttpConnection::reset_ring_buffer(ring_buffer &ring, char *storage, int capacity)
{
    ring.buffer = storage;
    ring.capacity = capacity;
    ring.head = 0;
    ring.tail = 0;
    ring.size = 0;
}

bool HttpConnection::ensure_read_capacity(int required_size)
{
    if (required_size <= 0)
    {
        required_size = READ_BUFFER_INITIAL_SIZE;
    }
    if (required_size > READ_BUFFER_MAX_SIZE)
    {
        return false;
    }
    if ((int)m_read_ring_buf.size() >= required_size && (int)m_read_buf.size() >= required_size + 1)
    {
        return true;
    }

    int new_capacity = m_read_ring_buf.empty() ? READ_BUFFER_INITIAL_SIZE : (int)m_read_ring_buf.size();
    while (new_capacity < required_size)
    {
        new_capacity *= 2;
        if (new_capacity > READ_BUFFER_MAX_SIZE)
        {
            new_capacity = READ_BUFFER_MAX_SIZE;
        }
        if (new_capacity < required_size && new_capacity == READ_BUFFER_MAX_SIZE)
        {
            return false;
        }
    }

    int old_size = m_read_ring.size;
    vector<char> new_ring_buf(new_capacity, '\0');
    if (old_size > 0 && m_read_ring.buffer != nullptr)
    {
        ring_peek(m_read_ring, new_ring_buf.data(), old_size);
    }
    m_read_ring_buf.swap(new_ring_buf);
    reset_ring_buffer(m_read_ring, m_read_ring_buf.data(), (int)m_read_ring_buf.size());
    m_read_ring.size = old_size;
    m_read_ring.tail = old_size;

    m_read_buf.assign(m_read_ring_buf.size() + 1, '\0');
    if (old_size > 0)
    {
        memcpy(m_read_buf.data(), m_read_ring_buf.data(), old_size);
    }
    return true;
}

bool HttpConnection::ensure_write_capacity(int required_size)
{
    if (required_size <= 0)
    {
        required_size = WRITE_BUFFER_INITIAL_SIZE;
    }
    if ((int)m_write_ring_buf.size() >= required_size && (int)m_write_buf.size() >= required_size + 1)
    {
        return true;
    }

    int new_capacity = m_write_ring_buf.empty() ? WRITE_BUFFER_INITIAL_SIZE : (int)m_write_ring_buf.size();
    while (new_capacity < required_size)
    {
        new_capacity *= 2;
    }

    int old_size = m_write_ring.size;
    vector<char> new_ring_buf(new_capacity, '\0');
    if (old_size > 0 && m_write_ring.buffer != nullptr)
    {
        ring_peek(m_write_ring, new_ring_buf.data(), old_size);
    }
    m_write_ring_buf.swap(new_ring_buf);
    reset_ring_buffer(m_write_ring, m_write_ring_buf.data(), (int)m_write_ring_buf.size());
    m_write_ring.size = old_size;
    m_write_ring.tail = old_size;

    m_write_buf.assign(m_write_ring_buf.size() + 1, '\0');
    if (old_size > 0)
    {
        memcpy(m_write_buf.data(), m_write_ring_buf.data(), old_size);
    }

    if ((int)m_file_send_buf.size() < new_capacity)
    {
        m_file_send_buf.assign(new_capacity, '\0');
    }
    return true;
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

int HttpConnection::ring_recv(ring_buffer &ring)
{
    if (ring.size >= ring.capacity)
    {
        return -1;
    }

    int writable = ring_writable(ring);
    int first = ring.capacity - ring.tail;
    if (first > writable)
    {
        first = writable;
    }

    int bytes_read = 0;
    if (m_https_enabled)
    {
        ERR_clear_error();
        bytes_read = SSL_read(m_ssl, ring.buffer + ring.tail, first);
        if (bytes_read <= 0)
        {
            int ssl_error = SSL_get_error(m_ssl, bytes_read);
            if (ssl_error == SSL_ERROR_WANT_READ)
            {
                m_tls_want_event = EPOLLIN;
                errno = EAGAIN;
            }
            else if (ssl_error == SSL_ERROR_WANT_WRITE)
            {
                m_tls_want_event = EPOLLOUT;
                errno = EAGAIN;
            }
            else if (ssl_error == SSL_ERROR_ZERO_RETURN)
            {
                return 0;
            }
            return -1;
        }
    }
    else
    {
        bytes_read = recv(m_sockfd, ring.buffer + ring.tail, first, 0);
    }
    if (bytes_read <= 0)
    {
        return bytes_read;
    }

    ring.tail = (ring.tail + bytes_read) % ring.capacity;
    ring.size += bytes_read;
    return bytes_read;
}

int HttpConnection::ring_peek(const ring_buffer &ring, char *dest, int len) const
{
    if (len > ring.size)
    {
        len = ring.size;
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

int HttpConnection::ring_send(ring_buffer &ring)
{
    if (ring.size == 0)
    {
        return 0;
    }

    int first = ring.capacity - ring.head;
    if (first > ring.size)
    {
        first = ring.size;
    }

    int bytes_sent = socket_send(ring.buffer + ring.head, first);
    if (bytes_sent <= 0)
    {
        return bytes_sent;
    }

    ring.head = (ring.head + bytes_sent) % ring.capacity;
    ring.size -= bytes_sent;
    if (ring.size == 0)
    {
        ring.head = 0;
        ring.tail = 0;
    }
    return bytes_sent;
}

int HttpConnection::socket_send(const char *buffer, int len)
{
    if (!m_https_enabled)
    {
        return send(m_sockfd, buffer, len, 0);
    }

    ERR_clear_error();
    int bytes_sent = SSL_write(m_ssl, buffer, len);
    if (bytes_sent > 0)
    {
        return bytes_sent;
    }

    int ssl_error = SSL_get_error(m_ssl, bytes_sent);
    if (ssl_error == SSL_ERROR_WANT_READ)
    {
        m_tls_want_event = EPOLLIN;
        errno = EAGAIN;
        return -1;
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE)
    {
        m_tls_want_event = EPOLLOUT;
        errno = EAGAIN;
        return -1;
    }
    if (ssl_error == SSL_ERROR_ZERO_RETURN)
    {
        return 0;
    }

    return -1;
}

int HttpConnection::wait_event_for_io(int default_event) const
{
    if (m_https_enabled)
    {
        return m_tls_want_event;
    }
    return default_event;
}

bool HttpConnection::send_file_over_tls()
{
    while (true)
    {
        if (m_file_send_offset >= m_file_send_size)
        {
            ssize_t file_read = read(m_filefd, m_file_send_buf.data(), m_file_send_buf.size());
            if (file_read < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                close_file();
                return false;
            }
            if (file_read == 0)
            {
                close_file();
                return true;
            }
            m_file_send_offset = 0;
            m_file_send_size = file_read;
        }

        int bytes_sent = socket_send(m_file_send_buf.data() + m_file_send_offset, m_file_send_size - m_file_send_offset);
        if (bytes_sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLOUT), m_TRIGMode);
                refresh_active();
                return true;
            }
            close_file();
            return false;
        }
        if (bytes_sent == 0)
        {
            close_file();
            return false;
        }

        m_file_send_offset += bytes_sent;
        bytes_have_send += bytes_sent;
        bytes_to_send -= bytes_sent;

        if (m_file_send_offset >= m_file_send_size)
        {
            m_file_send_offset = 0;
            m_file_send_size = 0;
        }

        if (bytes_to_send <= 0)
        {
            close_file();
            return true;
        }
    }
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

int HttpConnection::do_tls_handshake()
{
    if (!m_https_enabled || m_tls_handshake_done)
    {
        return 1;
    }

    ERR_clear_error();
    int ret = SSL_accept(m_ssl);
    if (ret == 1)
    {
        m_tls_handshake_done = true;
        m_tls_want_event = EPOLLIN;
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        refresh_active();
        return 1;
    }

    int ssl_error = SSL_get_error(m_ssl, ret);
    if (ssl_error == SSL_ERROR_WANT_READ)
    {
        m_tls_want_event = EPOLLIN;
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return 0;
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE)
    {
        m_tls_want_event = EPOLLOUT;
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
        return 0;
    }

    return -1;
}

bool HttpConnection::read_once()
{
    m_has_new_data = false;
    if (ring_writable(m_read_ring) <= 0 && !ensure_read_capacity(m_read_ring.capacity * 2))
    {
        return false;
    }

    if (0 == m_TRIGMode)
    {
        while (true)
        {
            int bytes_read = ring_recv(m_read_ring);
            if (bytes_read > 0)
            {
                m_has_new_data = true;
                continue;
            }

            if (bytes_read == 0)
            {
                return false;
            }

            if (errno == EINTR)
            {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (m_has_new_data)
                {
                    sync_read_buffer();
                    refresh_active();
                }
                modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
                return true;
            }

            return false;
        }
    }

    return read_once_et();
}

bool HttpConnection::read_once_et()
{
    m_has_new_data = false;
    bool has_read = false;
    int recvs_this_event = 0;

    while (ring_writable(m_read_ring) > 0)
    {
        int bytes_read = ring_recv(m_read_ring);
        if (bytes_read > 0)
        {
            has_read = true;
            m_has_new_data = true;
            ++recvs_this_event;
            if (recvs_this_event >= ET_READ_EVENT_MAX_RECVS)
            {
                sync_read_buffer();
                refresh_active();
                modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
                return true;
            }
            continue;
        }

        if (bytes_read == 0)
        {
            return false;
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if (has_read)
            {
                sync_read_buffer();
                refresh_active();
            }
            modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
            return has_read;
        }

        return false;
    }

    if (!ensure_read_capacity(m_read_ring.capacity * 2))
    {
        if (0 == m_close_log)
        {
            Log::get_instance()->write_log(2, "read buffer reached max size in ET mode, sockfd=%d", m_sockfd);
            Log::get_instance()->flush();
        }
        sync_read_buffer();
        return false;
    }
    return read_once_et();
}
