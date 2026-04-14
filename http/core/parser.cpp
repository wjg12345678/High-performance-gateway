#include "connection.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "../files/file_helpers.h"
#include "../../log/log.h"

using namespace std;

namespace
{
bool starts_with_ignore_case_local(const char *text, const char *prefix)
{
    return strncasecmp(text, prefix, strlen(prefix)) == 0;
}
}

HttpConnection::LINE_STATUS HttpConnection::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

HttpConnection::HTTP_CODE HttpConnection::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "HEAD") == 0)
        m_method = HEAD;
    else if (strcasecmp(method, "OPTIONS") == 0)
        m_method = OPTIONS;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else if (strcasecmp(method, "PUT") == 0)
        m_method = PUT;
    else if (strcasecmp(method, "DELETE") == 0)
        m_method = DELETE;
    else if (strcasecmp(method, "TRACE") == 0)
        m_method = TRACE;
    else if (strcasecmp(method, "CONNECT") == 0)
        m_method = CONNECT;
    else if (strcasecmp(method, "PATCH") == 0)
        m_method = PATH;
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") == 0)
    {
        m_is_http_1_1 = true;
        m_linger = true;
    }
    else if (strcasecmp(m_version, "HTTP/1.0") == 0)
    {
        m_is_http_1_1 = false;
        m_linger = false;
    }
    else
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    m_query_string = strchr(m_url, '?');
    if (m_query_string)
    {
        *m_query_string++ = '\0';
    }
    if (strlen(m_url) == 1)
        strcat(m_url, "index.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_is_http_1_1 && (!m_host || m_host[0] == '\0'))
        {
            return BAD_REQUEST;
        }
        if (m_chunked)
        {
            return NOT_IMPLEMENTED;
        }
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            m_body_start_idx = m_checked_idx;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
        else if (strcasecmp(text, "close") == 0)
        {
            m_linger = false;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
        if (m_content_length < 0)
        {
            return BAD_REQUEST;
        }
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");
        strncpy(m_content_type, text, sizeof(m_content_type) - 1);
    }
    else if (strncasecmp(text, "Transfer-Encoding:", 18) == 0)
    {
        text += 18;
        text += strspn(text, " \t");
        if (strcasecmp(text, "chunked") == 0)
        {
            m_chunked = true;
        }
    }
    else if (strncasecmp(text, "Authorization:", 14) == 0)
    {
        text += 14;
        text += strspn(text, " \t");
        m_authorization = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::parse_content(char *text)
{
    if (m_chunked)
    {
        return NOT_IMPLEMENTED;
    }
    (void)text;
    if (m_read_idx >= (m_body_start_idx + m_content_length))
    {
        m_read_buf[m_body_start_idx + m_content_length] = '\0';
        m_request_body.assign(m_read_buf.data() + m_body_start_idx, m_content_length);
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

bool HttpConnection::parse_post_body()
{
    m_form_data.clear();
    m_json_data.clear();

    if (m_request_body.empty())
    {
        return true;
    }

    if (starts_with_ignore_case_local(m_content_type, "application/x-www-form-urlencoded"))
    {
        return parse_form_urlencoded(m_request_body);
    }

    if (starts_with_ignore_case_local(m_content_type, "application/json"))
    {
        return parse_json_body(m_request_body);
    }

    if (starts_with_ignore_case_local(m_content_type, "multipart/form-data"))
    {
        return parse_multipart_form_data(m_request_body);
    }

    return true;
}

bool HttpConnection::parse_form_urlencoded(const string &body)
{
    size_t start = 0;
    while (start <= body.size())
    {
        size_t end = body.find('&', start);
        string item = body.substr(start, end == string::npos ? string::npos : end - start);
        if (!item.empty())
        {
            size_t eq = item.find('=');
            string key = url_decode(item.substr(0, eq));
            string value = eq == string::npos ? "" : url_decode(item.substr(eq + 1));
            if (!key.empty())
            {
                m_form_data[key] = value;
            }
        }

        if (end == string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool HttpConnection::parse_json_body(const string &body)
{
    string text = trim_copy(body);
    if (text.empty())
    {
        return true;
    }
    if (text.size() < 2 || text[0] != '{' || text[text.size() - 1] != '}')
    {
        return false;
    }

    size_t pos = 1;
    while (pos < text.size() - 1)
    {
        while (pos < text.size() - 1 && isspace((unsigned char)text[pos]))
        {
            ++pos;
        }
        if (pos >= text.size() - 1)
        {
            break;
        }
        if (text[pos] == ',')
        {
            ++pos;
            continue;
        }
        if (text[pos] != '"')
        {
            return false;
        }

        ++pos;
        string key;
        while (pos < text.size() - 1)
        {
            if (text[pos] == '\\')
            {
                ++pos;
                if (pos >= text.size() - 1)
                {
                    return false;
                }
            }
            else if (text[pos] == '"')
            {
                break;
            }
            key.push_back(text[pos]);
            ++pos;
        }
        if (pos >= text.size() - 1 || text[pos] != '"')
        {
            return false;
        }
        ++pos;

        while (pos < text.size() - 1 && isspace((unsigned char)text[pos]))
        {
            ++pos;
        }
        if (pos >= text.size() - 1 || text[pos] != ':')
        {
            return false;
        }
        ++pos;
        while (pos < text.size() - 1 && isspace((unsigned char)text[pos]))
        {
            ++pos;
        }
        if (pos >= text.size() - 1)
        {
            return false;
        }

        string value;
        if (text[pos] == '"')
        {
            ++pos;
            while (pos < text.size() - 1)
            {
                if (text[pos] == '\\')
                {
                    ++pos;
                    if (pos >= text.size() - 1)
                    {
                        return false;
                    }
                }
                else if (text[pos] == '"')
                {
                    break;
                }
                value.push_back(text[pos]);
                ++pos;
            }
            if (pos >= text.size() - 1 || text[pos] != '"')
            {
                return false;
            }
            ++pos;
        }
        else
        {
            size_t value_start = pos;
            while (pos < text.size() - 1 && text[pos] != ',' && text[pos] != '}')
            {
                ++pos;
            }
            value = trim_copy(text.substr(value_start, pos - value_start));
        }

        if (!key.empty())
        {
            m_json_data[key] = value;
        }

        while (pos < text.size() - 1 && isspace((unsigned char)text[pos]))
        {
            ++pos;
        }
        if (pos < text.size() - 1 && text[pos] == ',')
        {
            ++pos;
        }
    }

    return true;
}

bool HttpConnection::parse_multipart_form_data(const string &body)
{
    string content_type = m_content_type;
    size_t boundary_pos = content_type.find("boundary=");
    if (boundary_pos == string::npos)
    {
        return false;
    }

    string boundary = "--" + trim_copy(content_type.substr(boundary_pos + strlen("boundary=")));
    if (boundary.empty())
    {
        return false;
    }
    string boundary_delimiter = "\r\n" + boundary;

    size_t pos = 0;
    while (true)
    {
        size_t boundary_start = (pos == 0) ? body.find(boundary, pos) : body.find(boundary_delimiter, pos);
        if (boundary_start == string::npos)
        {
            break;
        }
        if (pos != 0)
        {
            boundary_start += 2;
        }

        size_t part_start = boundary_start + boundary.size();
        if (part_start + 1 < body.size() && body.compare(part_start, 2, "--") == 0)
        {
            break;
        }
        if (part_start + 2 <= body.size() && body.compare(part_start, 2, "\r\n") == 0)
        {
            part_start += 2;
        }

        size_t header_end = body.find("\r\n\r\n", part_start);
        if (header_end == string::npos)
        {
            return false;
        }

        string header_block = body.substr(part_start, header_end - part_start);
        size_t next_boundary = body.find(boundary_delimiter, header_end + 4);
        if (next_boundary == string::npos)
        {
            return false;
        }

        size_t value_end = next_boundary;
        string value = body.substr(header_end + 4, value_end - (header_end + 4));

        string field_name;
        string field_filename;
        string field_content_type;
        size_t line_start = 0;
        while (line_start <= header_block.size())
        {
            size_t line_end = header_block.find("\r\n", line_start);
            string line = header_block.substr(line_start, line_end == string::npos ? string::npos : line_end - line_start);
            if (starts_with_ignore_case_local(line.c_str(), "Content-Disposition:"))
            {
                field_name = header_param_value(line, "name");
                field_filename = header_param_value(line, "filename");
            }
            else if (starts_with_ignore_case_local(line.c_str(), "Content-Type:"))
            {
                field_content_type = trim_copy(line.substr(strlen("Content-Type:")));
            }

            if (line_end == string::npos)
            {
                break;
            }
            line_start = line_end + 2;
        }

        if (!field_name.empty())
        {
            if (!field_filename.empty())
            {
                m_form_data[field_name] = value;
                m_form_data["filename"] = http_file_helpers::sanitize_filename(field_filename);
                m_form_data["content"] = value;
                if (!field_content_type.empty())
                {
                    m_form_data[field_name + "_content_type"] = field_content_type;
                    m_form_data["content_type"] = field_content_type;
                }
            }
            else
            {
                m_form_data[field_name] = value;
            }
        }

        pos = next_boundary;
    }

    return !m_form_data.empty();
}

HttpConnection::HTTP_CODE HttpConnection::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = nullptr;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST || ret == NOT_IMPLEMENTED)
                return ret;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST || ret == NOT_IMPLEMENTED)
                return ret;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == NOT_IMPLEMENTED)
                return ret;
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}
