#include "connection.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace std;

namespace
{
const char *kOk200Title = "OK";
const char *kError400Title = "Bad Request";
const char *kError400Form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *kError403Title = "Forbidden";
const char *kError403Form = "You do not have permission to get file form this server.\n";
const char *kError404Title = "Not Found";
const char *kError404Form = "The requested file was not found on this server.\n";
const char *kError501Title = "Not Implemented";
const char *kError501Form = "The requested HTTP feature is not implemented.\n";
const char *kError500Title = "Internal Error";
const char *kError500Form = "There was an unusual problem serving the request file.\n";
}

void HttpConnection::close_file()
{
    if (m_filefd != -1)
    {
        close(m_filefd);
        m_filefd = -1;
    }
}

bool HttpConnection::add_response(const char *format, ...)
{
    va_list arg_list;
    va_start(arg_list, format);
    va_list arg_list_copy;
    va_copy(arg_list_copy, arg_list);
    int len = vsnprintf(nullptr, 0, format, arg_list_copy);
    va_end(arg_list_copy);
    if (len < 0)
    {
        va_end(arg_list);
        return false;
    }

    if (!ensure_write_capacity(m_write_ring.size + len))
    {
        va_end(arg_list);
        return false;
    }

    vector<char> temp_buf(len + 1, '\0');
    if (vsnprintf(temp_buf.data(), temp_buf.size(), format, arg_list) != len)
    {
        va_end(arg_list);
        return false;
    }
    va_end(arg_list);

    if (ring_append(m_write_ring, temp_buf.data(), len) < 0)
    {
        return false;
    }
    m_write_idx = m_write_ring.size;
    ring_peek(m_write_ring, m_write_buf.data(), m_write_ring.size);
    m_write_buf[m_write_ring.size] = '\0';
    return true;
}

bool HttpConnection::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool HttpConnection::add_headers(int content_len, const char *content_type)
{
    if (!add_content_length(content_len) || !add_content_type(content_type) ||
        !add_linger() || !add_keep_alive() || !add_allow())
    {
        return false;
    }
    if (!m_extra_headers.empty() && !add_response("%s", m_extra_headers.c_str()))
    {
        return false;
    }
    return add_blank_line();
}

bool HttpConnection::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool HttpConnection::add_content_type(const char *content_type)
{
    return add_response("Content-Type:%s\r\n", content_type);
}

bool HttpConnection::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool HttpConnection::add_keep_alive()
{
    if (!m_linger)
    {
        return true;
    }
    return add_response("Keep-Alive:%s\r\n", "timeout=15, max=100");
}

bool HttpConnection::add_allow()
{
    return add_response("Allow:%s\r\n", "GET, POST, HEAD, OPTIONS, DELETE");
}

bool HttpConnection::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool HttpConnection::add_content(const char *content)
{
    return add_response("%s", content);
}

bool HttpConnection::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, kError500Title);
        add_headers(strlen(kError500Form));
        if (!add_content(kError500Form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400, kError400Title);
        add_headers(strlen(kError400Form), "text/plain");
        if (!add_content(kError400Form))
            return false;
        break;
    }
    case NO_RESOURCE:
    {
        add_status_line(404, kError404Title);
        add_headers(strlen(kError404Form), "text/plain");
        if (!add_content(kError404Form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, kError403Title);
        add_headers(strlen(kError403Form), "text/plain");
        if (!add_content(kError403Form))
            return false;
        break;
    }
    case NOT_IMPLEMENTED:
    {
        add_status_line(501, kError501Title);
        add_headers(strlen(kError501Form), "text/plain");
        if (!add_content(kError501Form))
            return false;
        break;
    }
    case OPTIONS_REQUEST:
    {
        add_status_line(204, kOk200Title);
        add_headers(0, "text/plain");
        bytes_to_send = m_write_idx;
        m_header_bytes_sent = 0;
        m_file_offset = 0;
        return true;
    }
    case MEMORY_REQUEST:
    {
        add_status_line(m_response_status, m_response_status_title);
        add_headers(m_response_body_len, m_response_content_type);
        if (m_response_body_len > 0 && !add_content(m_response_body))
        {
            return false;
        }
        bytes_to_send = m_write_idx;
        m_header_bytes_sent = 0;
        m_file_offset = 0;
        return true;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, kOk200Title);
        if (m_method == HEAD)
        {
            add_headers(m_file_stat.st_size, m_response_content_type[0] ? m_response_content_type : "application/octet-stream");
            close_file();
            bytes_to_send = m_write_idx;
            m_header_bytes_sent = 0;
            m_file_offset = 0;
            return true;
        }
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size, m_response_content_type[0] ? m_response_content_type : "application/octet-stream");
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            m_header_bytes_sent = 0;
            m_file_offset = 0;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
        break;
    }
    default:
        return false;
    }
    bytes_to_send = m_write_idx;
    m_header_bytes_sent = 0;
    m_file_offset = 0;
    return true;
}
