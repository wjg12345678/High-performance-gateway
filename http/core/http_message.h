#ifndef ATLAS_HTTP_CORE_HTTP_MESSAGE_H
#define ATLAS_HTTP_CORE_HTTP_MESSAGE_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>

typedef struct MYSQL MYSQL;

namespace http_core
{
enum Method
{
    GET = 0,
    POST,
    HEAD,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT,
    PATH
};

enum HttpCode
{
    NO_REQUEST,
    GET_REQUEST,
    BAD_REQUEST,
    NO_RESOURCE,
    FORBIDDEN_REQUEST,
    FILE_REQUEST,
    INTERNAL_ERROR,
    CLOSED_CONNECTION,
    OPTIONS_REQUEST,
    NOT_IMPLEMENTED,
    PAYLOAD_TOO_LARGE,
    MEMORY_REQUEST
};

struct UploadedFile
{
    std::string temp_path;
    std::string filename;
    std::string content_type;
    std::string sha256;
    long size;
    bool released;

    UploadedFile() : size(0), released(false) {}
    bool has_file() const { return !temp_path.empty(); }
};

struct HttpRequest
{
    Method method;
    std::string path;
    std::string query_string;
    std::string version;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> form;
    std::map<std::string, std::string> json;
    long content_length;
    bool content_length_seen;
    bool keep_alive;
    bool http_1_1;
    bool chunked;
    std::string body;
    UploadedFile upload;

    HttpRequest();

    bool is_api_request() const;
    bool requires_auth() const;
    bool should_skip_request_log() const;
    const char *method_name() const;
    std::string header_value(const std::string &key) const;
    std::string value(const std::string &primary, const std::string &fallback = "") const;
    std::string query_value(const std::string &key) const;
    long query_long_value(const std::string &key, long fallback, long minimum, long maximum) const;
    bool query_truthy_value(const std::string &key) const;
    std::string bearer_token() const;
};

struct FileResponse
{
    bool enabled;
    std::string path;
    std::string content_type;
    std::string download_name;

    FileResponse() : enabled(false) {}
};

struct HttpResponse
{
    int status;
    std::string title;
    std::string content_type;
    std::string body;
    std::map<std::string, std::string> headers;
    bool options_response;
    FileResponse file;

    HttpResponse();

    void set_body(int status_code, const char *status_title, const std::string &response_body,
                  const char *response_content_type);
    void set_json_error(int status_code, const char *status_title, const std::string &message);
    void set_file(const std::string &file_path, const std::string &file_content_type,
                  const std::string &download_name);
    void set_options();
};

struct RequestContext
{
    MYSQL *mysql;
    std::string current_user;
    std::string doc_root;
    size_t upload_max_bytes;
    size_t user_storage_quota_bytes;
    std::string client_ip;
    std::string user_agent;
    bool legacy_compat_enabled;
    int close_log;
    std::vector<std::string> released_temp_upload_paths;

    RequestContext();
    void release_temp_upload(const std::string &path);
    void release_temp_upload(UploadedFile &upload);
};

const char *method_name(Method method);
std::string url_decode(const std::string &value);
std::string trim_copy(const std::string &value);
std::string json_escape(const std::string &value);
bool decode_base64(const std::string &input, std::string &output);
std::string header_param_value(const std::string &header_line, const std::string &key);
std::string make_session_token();
bool starts_with_ignore_case(const std::string &text, const char *prefix);
bool equals_ignore_case(const std::string &left, const char *right);
std::string lowercase_copy(const std::string &value);
}

#endif
