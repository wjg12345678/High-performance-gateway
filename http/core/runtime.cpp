#include "connection.h"

#include <cerrno>
#include <sys/sendfile.h>

void modfd(int epollfd, int fd, int ev, int TRIGMode);

void HttpConnection::reset_request_parser_state()
{
    reset_ring_buffer(m_read_ring, m_read_ring_buf.data(), (int)m_read_ring_buf.size());
    m_read_idx = 0;
    m_checked_idx = 0;
    m_body_start_idx = 0;
    m_start_line = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    if (!m_read_buf.empty())
    {
        m_read_buf[0] = '\0';
    }
}

bool HttpConnection::finish_write_cycle()
{
    close_file();
    modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
    refresh_active();

    if (!m_linger)
    {
        return false;
    }

    init();
    return true;
}

bool HttpConnection::write()
{
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, wait_event_for_io(EPOLLIN), m_TRIGMode);
        init();
        refresh_active();
        return true;
    }

    while (1)
    {
        if (m_write_ring.size > 0)
        {
            int header_written = ring_send(m_write_ring);
            if (header_written < 0)
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
            if (header_written == 0)
            {
                close_file();
                return false;
            }

            m_header_bytes_sent += header_written;
            bytes_have_send += header_written;
            bytes_to_send -= header_written;
            if (bytes_to_send <= 0)
            {
                return finish_write_cycle();
            }
            continue;
        }

        if (m_filefd != -1)
        {
            if (m_https_enabled)
            {
                if (!send_file_over_tls())
                {
                    return false;
                }
                if (bytes_to_send > 0)
                {
                    return true;
                }
            }
            else
            {
                ssize_t file_written = sendfile(m_sockfd, m_filefd, &m_file_offset, bytes_to_send);
                if (file_written < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                        refresh_active();
                        return true;
                    }
                    close_file();
                    return false;
                }
                if (file_written == 0)
                {
                    if (bytes_to_send <= 0 || m_file_offset >= m_file_stat.st_size)
                    {
                        bytes_to_send = 0;
                        continue;
                    }
                    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                    refresh_active();
                    return true;
                }

                bytes_have_send += file_written;
                bytes_to_send -= file_written;
            }
        }

        if (bytes_to_send <= 0)
        {
            return finish_write_cycle();
        }
    }
}

void HttpConnection::process()
{
    if (bytes_to_send > 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
        return;
    }
    if (!m_has_new_data)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    HTTP_CODE read_ret = process_read();
    m_has_new_data = false;
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    reset_request_parser_state();

    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
        return;
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
