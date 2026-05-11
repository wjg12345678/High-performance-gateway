#include "file_controller.h"

#include "../files/file_helpers.h"
#include "../../infra/storage/storage.h"
#include "../../repo/mysql/file_repository.h"
#include "../../repo/mysql/operation_repository.h"
#include "../../service/files/file_service.h"
#include "../../service/files/share_service.h"
#include "../../service/files/upload_service.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace std;

namespace
{
const int kDefaultListLimit = 20;
const int kMaxListLimit = 100;

http_core::HttpCode require_user_session(const http_core::RequestContext &context,
                                         http_core::HttpResponse &response,
                                         const char *message)
{
    if (!context.current_user.empty())
    {
        return http_core::NO_REQUEST;
    }
    response.set_json_error(403, "Forbidden", message);
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode respond_json_error(http_core::HttpResponse &response, int status,
                                       const char *title, const string &message)
{
    response.set_json_error(status, title, message);
    return http_core::MEMORY_REQUEST;
}

int service_status(service_files::ErrorCode code)
{
    switch (code)
    {
    case service_files::ErrorCode::None: return 200;
    case service_files::ErrorCode::InvalidArgument: return 400;
    case service_files::ErrorCode::Forbidden: return 403;
    case service_files::ErrorCode::NotFound: return 404;
    case service_files::ErrorCode::Conflict: return 409;
    case service_files::ErrorCode::Gone: return 410;
    case service_files::ErrorCode::TooManyRequests: return 429;
    case service_files::ErrorCode::PayloadTooLarge: return 413;
    case service_files::ErrorCode::Internal:
    default: return 500;
    }
}

const char *service_title(service_files::ErrorCode code)
{
    switch (code)
    {
    case service_files::ErrorCode::None: return "OK";
    case service_files::ErrorCode::InvalidArgument: return "Bad Request";
    case service_files::ErrorCode::Forbidden: return "Forbidden";
    case service_files::ErrorCode::NotFound: return "Not Found";
    case service_files::ErrorCode::Conflict: return "Conflict";
    case service_files::ErrorCode::Gone: return "Gone";
    case service_files::ErrorCode::TooManyRequests: return "Too Many Requests";
    case service_files::ErrorCode::PayloadTooLarge: return "Payload Too Large";
    case service_files::ErrorCode::Internal:
    default: return "Internal Error";
    }
}

http_core::HttpCode respond_service_error(http_core::HttpResponse &response,
                                          const service_files::ServiceError &error)
{
    if (error.message.empty())
    {
        return http_core::INTERNAL_ERROR;
    }
    return respond_json_error(response, service_status(error.code), service_title(error.code), error.message);
}

bool write_operation_log(MYSQL *mysql, const string &username, const string &action,
                         const string &resource_type, long resource_id, const string &detail)
{
    return repo_mysql::insert_operation_log(mysql, username, action, resource_type, resource_id, detail);
}

string trim_ascii_copy(const string &value)
{
    size_t start = 0;
    while (start < value.size() && isspace(static_cast<unsigned char>(value[start])))
    {
        ++start;
    }
    size_t end = value.size();
    while (end > start && isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(start, end - start);
}

bool parse_non_negative_long(const string &value, long &number)
{
    const string trimmed = trim_ascii_copy(value);
    if (trimmed.empty())
    {
        return false;
    }

    char *endptr = nullptr;
    number = strtol(trimmed.c_str(), &endptr, 10);
    return endptr != trimmed.c_str() && endptr != nullptr && *endptr == '\0' && number >= 0;
}

bool is_valid_share_token(const string &token)
{
    if (token.empty() || token.size() > 64)
    {
        return false;
    }
    for (size_t i = 0; i < token.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(token[i]);
        if (!isalnum(ch) && ch != '-' && ch != '_')
        {
            return false;
        }
    }
    return true;
}

bool parse_share_path(const char *path, bool download, string &token)
{
    const string suffix = path == nullptr ? string() : string(path);
    const string download_suffix = "/download";
    if (download)
    {
        if (suffix.size() <= download_suffix.size() ||
            suffix.compare(suffix.size() - download_suffix.size(), download_suffix.size(), download_suffix) != 0)
        {
            return false;
        }
        token = suffix.substr(0, suffix.size() - download_suffix.size());
    }
    else
    {
        if (suffix.find('/') != string::npos)
        {
            return false;
        }
        token = suffix;
    }
    return is_valid_share_token(token);
}

bool parse_managed_upload_payload(const http_core::HttpRequest &request,
                                  const http_core::RequestContext &context,
                                  service_files::UploadPayload &payload,
                                  int &status,
                                  const char *&title,
                                  string &message)
{
    payload.filename = http_file_helpers::sanitize_filename(request.value("filename", "name"));
    payload.content_type = request.value("content_type", "file_content_type");
    const string folder_value = request.value("folder_id");
    if (!folder_value.empty())
    {
        char *endptr = nullptr;
        const long folder_id = strtol(folder_value.c_str(), &endptr, 10);
        if (endptr == folder_value.c_str() || (endptr != nullptr && *endptr != '\0') || folder_id < 0)
        {
            status = 400;
            title = "Bad Request";
            message = "invalid folder_id";
            return false;
        }
        payload.folder_id = folder_id;
    }

    if (request.upload.has_file())
    {
        if (payload.filename.empty())
        {
            payload.filename = http_file_helpers::sanitize_filename(request.upload.filename);
        }
        if (payload.filename.empty())
        {
            payload.filename = "file.bin";
        }
        if (payload.content_type.empty())
        {
            payload.content_type = request.upload.content_type.empty() ? "application/octet-stream"
                                                                       : request.upload.content_type;
        }

        payload.temp_path = request.upload.temp_path;
        payload.sha256 = request.upload.sha256;
        payload.size = request.upload.size;
        payload.use_temp_file = true;
        return true;
    }

    if (!context.legacy_compat_enabled)
    {
        status = 400;
        title = "Bad Request";
        message = "multipart/form-data file upload required";
        return false;
    }

    payload.content = request.value("content", "file");
    if (payload.content.empty())
    {
        const string content_base64 = request.value("content_base64", "file_base64");
        if (!content_base64.empty() && !http_core::decode_base64(content_base64, payload.content))
        {
            status = 400;
            title = "Bad Request";
            message = "invalid base64 content";
            return false;
        }
    }

    if (payload.content.empty())
    {
        status = 400;
        title = "Bad Request";
        message = "content is empty";
        return false;
    }

    if (payload.filename.empty())
    {
        payload.filename = "file.txt";
    }
    if (payload.content_type.empty())
    {
        payload.content_type = "text/plain";
    }
    if (payload.content.size() > context.upload_max_bytes)
    {
        status = 413;
        title = "Payload Too Large";
        message = string("upload exceeds limit of ") + to_string(context.upload_max_bytes) + " bytes";
        return false;
    }

    payload.size = payload.content.size();
    return true;
}

service_files::UploadQuota upload_quota(const http_core::RequestContext &context)
{
    service_files::UploadQuota quota;
    quota.max_single_file_bytes = static_cast<long>(context.upload_max_bytes);
    quota.max_total_bytes = static_cast<long>(context.user_storage_quota_bytes);
    return quota;
}
}

namespace http_controllers
{
http_core::HttpCode FileController::upload_preflight(http_core::HttpRequest &request,
                                                     http_core::RequestContext &context,
                                                     http_core::HttpResponse &response)
{
    http_core::HttpCode auth_code = require_user_session(context, response, "file upload requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    string size_value = request.value("size", "file_size");
    if (size_value.empty())
    {
        size_value = request.value("bytes");
    }
    if (size_value.empty())
    {
        size_value = request.query_value("size");
    }

    long requested_bytes = 0;
    if (!parse_non_negative_long(size_value, requested_bytes))
    {
        return respond_json_error(response, 400, "Bad Request", "invalid file size");
    }

    const string request_folder = request.value("folder_id");
    const string folder_value = request_folder.empty() ? request.query_value("folder_id") : request_folder;
    if (!folder_value.empty())
    {
        long folder_id = 0;
        if (!parse_non_negative_long(folder_value, folder_id))
        {
            return respond_json_error(response, 400, "Bad Request", "invalid folder_id");
        }

        bool exists = false;
        if (!repo_mysql::folder_exists(context.mysql, context.current_user, folder_id, exists))
        {
            return http_core::INTERNAL_ERROR;
        }
        if (!exists)
        {
            return respond_json_error(response, 404, "Not Found", "folder not found");
        }
    }

    service_files::UploadQuotaStatus quota_status;
    service_files::ServiceError error;
    if (!service_files::inspect_upload_quota(context.mysql, context.current_user, requested_bytes,
                                             upload_quota(context), quota_status, error))
    {
        return respond_service_error(response, error);
    }

    response.set_body(200, "OK", service_files::build_upload_preflight_json(quota_status), "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::upload(http_core::HttpRequest &request,
                                           http_core::RequestContext &context,
                                           http_core::HttpResponse &response)
{
    http_core::HttpCode auth_code = require_user_session(context, response, "file upload requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    service_files::UploadPayload payload;
    int error_status = 200;
    const char *error_title = "OK";
    string error_message;
    if (!parse_managed_upload_payload(request, context, payload, error_status, error_title, error_message))
    {
        return respond_json_error(response, error_status, error_title, error_message);
    }

    const bool is_public = service_files::parse_public_flag(request.value("is_public"));
    const string stored_name_prefix = http_core::make_session_token();
    if (stored_name_prefix.empty())
    {
        return http_core::INTERNAL_ERROR;
    }

    service_files::UploadResult result;
    service_files::ServiceError error;
    if (!service_files::create_uploaded_file(context.mysql, context.doc_root, context.current_user,
                                             stored_name_prefix, payload, is_public, upload_quota(context),
                                             result, &error))
    {
        return respond_service_error(response, error);
    }
    if (payload.use_temp_file && payload.temp_path == request.upload.temp_path)
    {
        context.release_temp_upload(request.upload);
    }

    write_operation_log(context.mysql, context.current_user, "upload", "file", result.file_id, result.filename);
    response.set_body(200, "OK", service_files::build_upload_success_json(result), "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::drive_item_list(http_core::HttpRequest &request,
                                                    http_core::RequestContext &context,
                                                    http_core::HttpResponse &response)
{
    http_core::HttpCode auth_code = require_user_session(context, response, "drive list requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    const long folder_id = request.query_long_value("folder_id", 0, 0, 2147483647L);
    string body;
    service_files::ServiceError error;
    if (!service_files::list_drive_items(context.mysql, context.current_user, folder_id, body, error))
    {
        return respond_service_error(response, error);
    }

    response.set_body(200, "OK", body, "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::trash_item_list(http_core::HttpRequest &request,
                                                    http_core::RequestContext &context,
                                                    http_core::HttpResponse &response)
{
    (void)request;
    http_core::HttpCode auth_code = require_user_session(context, response, "trash list requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    string body;
    if (!service_files::list_trash_items(context.mysql, context.current_user, body))
    {
        return http_core::INTERNAL_ERROR;
    }

    response.set_body(200, "OK", body, "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::empty_trash(http_core::HttpRequest &request,
                                                http_core::RequestContext &context,
                                                http_core::HttpResponse &response)
{
    (void)request;
    http_core::HttpCode auth_code = require_user_session(context, response, "trash empty requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    service_files::EmptyTrashResult result;
    service_files::ServiceError error;
    if (!service_files::empty_trash(context.mysql, context.doc_root, context.current_user, result, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, "empty_trash", "file", 0,
                        to_string(result.deleted_count) + " files");
    response.set_body(200, "OK",
                      "{\"code\":0,\"message\":\"trash emptied\",\"deleted_count\":" +
                          to_string(result.deleted_count) + "}",
                      "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::drive_folder_create(http_core::HttpRequest &request,
                                                        http_core::RequestContext &context,
                                                        http_core::HttpResponse &response)
{
    http_core::HttpCode auth_code = require_user_session(context, response, "folder create requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    const string parent_value = request.value("parent_id", "folder_id");
    long parent_id = 0;
    if (!parent_value.empty() && !parse_non_negative_long(parent_value, parent_id))
    {
        return respond_json_error(response, 400, "Bad Request", "invalid parent_id");
    }

    repo_mysql::FolderListItem folder;
    service_files::ServiceError error;
    if (!service_files::create_folder(context.mysql, context.current_user, parent_id,
                                      request.value("name"), folder, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, "create_folder", "folder", folder.id, folder.name);
    response.set_body(200, "OK", service_files::build_folder_created_json(folder), "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::drive_folder_delete(http_core::HttpRequest &request,
                                                        http_core::RequestContext &context,
                                                        http_core::HttpResponse &response,
                                                        const char *path)
{
    (void)request;
    http_core::HttpCode auth_code = require_user_session(context, response, "folder delete requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long folder_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return http_core::BAD_REQUEST;
    }

    repo_mysql::FolderListItem folder;
    service_files::ServiceError error;
    if (!service_files::delete_empty_folder(context.mysql, context.current_user, folder_id, folder, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, "delete_folder", "folder", folder_id, folder.name);
    response.set_body(200, "OK", "{\"code\":0,\"message\":\"folder deleted\"}", "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::drive_file_upload(http_core::HttpRequest &request,
                                                      http_core::RequestContext &context,
                                                      http_core::HttpResponse &response)
{
    return upload(request, context, response);
}

http_core::HttpCode FileController::public_file_list(http_core::HttpRequest &request,
                                                     http_core::RequestContext &context,
                                                     http_core::HttpResponse &response)
{
    const long limit = request.query_long_value("limit", kDefaultListLimit, 1, kMaxListLimit);
    const long cursor = request.query_long_value("cursor", 0, 0, 2147483647L);

    string body;
    if (!service_files::list_public_files(context.mysql, cursor, limit, body))
    {
        return http_core::INTERNAL_ERROR;
    }

    response.set_body(200, "OK", body, "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::public_file_detail(http_core::HttpRequest &request,
                                                       http_core::RequestContext &context,
                                                       http_core::HttpResponse &response,
                                                       const char *path)
{
    (void)request;
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return http_core::BAD_REQUEST;
    }

    ManagedFileRecord record;
    long actual_size = 0;
    service_files::ServiceError error;
    if (!service_files::load_public_file_detail(context.mysql, context.doc_root, file_id, record, actual_size, error))
    {
        return respond_service_error(response, error);
    }

    response.set_body(200, "OK", service_files::build_public_file_detail_json(record, actual_size),
                      "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::private_file_download(http_core::HttpRequest &request,
                                                          http_core::RequestContext &context,
                                                          http_core::HttpResponse &response,
                                                          const char *path)
{
    (void)request;
    http_core::HttpCode auth_code = require_user_session(context, response, "file download requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return http_core::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::load_owned_file_record(context.mysql, context.current_user, file_id, record, error))
    {
        return respond_service_error(response, error);
    }

    response.set_file(infra_storage::file_path(context.doc_root, record.stored_name),
                      record.content_type, record.original_name);
    write_operation_log(context.mysql, context.current_user, "download", "file", file_id, record.original_name);
    return http_core::FILE_REQUEST;
}

http_core::HttpCode FileController::public_file_download(http_core::HttpRequest &request,
                                                         http_core::RequestContext &context,
                                                         http_core::HttpResponse &response,
                                                         const char *path)
{
    (void)request;
    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/download") != 0)
    {
        return http_core::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::load_public_file_record(context.mysql, file_id, record, error))
    {
        return respond_service_error(response, error);
    }

    response.set_file(infra_storage::file_path(context.doc_root, record.stored_name),
                      record.content_type, record.original_name);
    write_operation_log(context.mysql, context.current_user.empty() ? "guest" : context.current_user,
                        "public_download", "file", file_id, record.original_name);
    return http_core::FILE_REQUEST;
}

http_core::HttpCode FileController::share_create(http_core::HttpRequest &request,
                                                 http_core::RequestContext &context,
                                                 http_core::HttpResponse &response,
                                                 const char *path)
{
    http_core::HttpCode auth_code = require_user_session(context, response, "file share requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/share") != 0)
    {
        return http_core::BAD_REQUEST;
    }

    service_files::ShareOptions options;
    options.access_code = request.value("access_code", "code");

    const string expires_value = request.value("expires_in_seconds", "expires_in");
    if (!expires_value.empty() && !parse_non_negative_long(expires_value, options.expires_in_seconds))
    {
        return respond_json_error(response, 400, "Bad Request", "invalid expires_in_seconds");
    }
    const string max_downloads_value = request.value("max_downloads", "download_limit");
    if (!max_downloads_value.empty() && !parse_non_negative_long(max_downloads_value, options.max_downloads))
    {
        return respond_json_error(response, 400, "Bad Request", "invalid max_downloads");
    }

    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::create_share_link(context.mysql, context.current_user, file_id, options, share, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, "create_share", "file", file_id, share.filename);
    response.set_body(200, "OK", service_files::build_share_json(share), "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::share_detail(http_core::HttpRequest &request,
                                                 http_core::RequestContext &context,
                                                 http_core::HttpResponse &response,
                                                 const char *path)
{
    string token;
    if (!parse_share_path(path, false, token))
    {
        return http_core::BAD_REQUEST;
    }

    string access_code = request.value("access_code", "code");
    if (access_code.empty())
    {
        access_code = request.query_value("access_code");
    }
    if (access_code.empty())
    {
        access_code = request.query_value("code");
    }

    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::load_share_detail(context.mysql, token, access_code, share, error))
    {
        return respond_service_error(response, error);
    }

    response.set_body(200, "OK", service_files::build_share_json(share), "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::share_download(http_core::HttpRequest &request,
                                                   http_core::RequestContext &context,
                                                   http_core::HttpResponse &response,
                                                   const char *path)
{
    string token;
    if (!parse_share_path(path, true, token))
    {
        return http_core::BAD_REQUEST;
    }

    string access_code = request.value("access_code", "code");
    if (access_code.empty())
    {
        access_code = request.query_value("access_code");
    }
    if (access_code.empty())
    {
        access_code = request.query_value("code");
    }

    ManagedFileRecord record;
    service_files::ShareResult share;
    service_files::ServiceError error;
    if (!service_files::load_share_download(context.mysql, token, access_code, record, share, error))
    {
        return respond_service_error(response, error);
    }

    response.set_file(infra_storage::file_path(context.doc_root, record.stored_name),
                      record.content_type, record.original_name);
    write_operation_log(context.mysql, "guest", "share_download", "file", record.file_id, record.original_name);
    return http_core::FILE_REQUEST;
}

http_core::HttpCode FileController::remove(http_core::HttpRequest &request,
                                           http_core::RequestContext &context,
                                           http_core::HttpResponse &response,
                                           const char *path)
{
    (void)request;
    http_core::HttpCode auth_code = require_user_session(context, response, "file delete requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || *endptr != '\0')
    {
        return http_core::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::soft_delete_file(context.mysql, context.current_user, file_id, record, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, "delete", "file", file_id, record.original_name);
    response.set_body(200, "OK", "{\"code\":0,\"message\":\"file moved to recycle bin\"}",
                      "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::restore(http_core::HttpRequest &request,
                                            http_core::RequestContext &context,
                                            http_core::HttpResponse &response,
                                            const char *path)
{
    (void)request;
    http_core::HttpCode auth_code = require_user_session(context, response, "file restore requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/restore") != 0)
    {
        return http_core::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::restore_file(context.mysql, context.current_user, file_id, record, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, "restore", "file", file_id, record.original_name);
    response.set_body(200, "OK", service_files::build_file_restored_json(record), "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::remove_permanently(http_core::HttpRequest &request,
                                                       http_core::RequestContext &context,
                                                       http_core::HttpResponse &response,
                                                       const char *path)
{
    (void)request;
    http_core::HttpCode auth_code = require_user_session(context, response, "file permanent delete requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/permanent") != 0)
    {
        return http_core::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    if (!service_files::hard_delete_file(context.mysql, context.doc_root, context.current_user, file_id, record, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, "purge", "file", file_id, record.original_name);
    response.set_body(200, "OK", "{\"code\":0,\"message\":\"file permanently deleted\"}",
                      "application/json");
    return http_core::MEMORY_REQUEST;
}

http_core::HttpCode FileController::update_visibility(http_core::HttpRequest &request,
                                                      http_core::RequestContext &context,
                                                      http_core::HttpResponse &response,
                                                      const char *path)
{
    http_core::HttpCode auth_code = require_user_session(context, response, "file visibility update requires user session");
    if (auth_code != http_core::NO_REQUEST)
    {
        return auth_code;
    }

    char *endptr = nullptr;
    long file_id = strtol(path, &endptr, 10);
    if (endptr == path || strcmp(endptr, "/visibility") != 0)
    {
        return http_core::BAD_REQUEST;
    }

    ManagedFileRecord record;
    service_files::ServiceError error;
    const bool is_public = service_files::parse_public_flag(request.value("is_public"));
    if (!service_files::update_file_visibility(context.mysql, context.current_user, file_id, is_public, record, error))
    {
        return respond_service_error(response, error);
    }

    write_operation_log(context.mysql, context.current_user, is_public ? "publish" : "unpublish",
                        "file", file_id, record.original_name);
    response.set_body(200, "OK",
                      is_public ? "{\"code\":0,\"message\":\"file is now public\"}"
                                : "{\"code\":0,\"message\":\"file is now private\"}",
                      "application/json");
    return http_core::MEMORY_REQUEST;
}
}
