#include "connection.h"

#include <cstring>

using namespace std;

string HttpConnection::url_decode(const string &value) const
{
    return http_core::url_decode(value);
}

string HttpConnection::trim_copy(const string &value) const
{
    return http_core::trim_copy(value);
}

string HttpConnection::json_escape(const string &value) const
{
    return http_core::json_escape(value);
}

bool HttpConnection::decode_base64(const string &input, string &output) const
{
    return http_core::decode_base64(input, output);
}

string HttpConnection::header_param_value(const string &header_line, const string &key) const
{
    return http_core::header_param_value(header_line, key);
}

void HttpConnection::set_memory_response(int status, const char *title, const string &body, const char *content_type)
{
    m_response_status = status;
    strncpy(m_response_status_title, title, sizeof(m_response_status_title) - 1);
    m_response_status_title[sizeof(m_response_status_title) - 1] = '\0';
    strncpy(m_response_content_type, content_type, sizeof(m_response_content_type) - 1);
    m_response_content_type[sizeof(m_response_content_type) - 1] = '\0';
    m_response_body_storage = body;
    m_response_body = m_response_body_storage.c_str();
    m_response_body_len = m_response_body_storage.size();
}
