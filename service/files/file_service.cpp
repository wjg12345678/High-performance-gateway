#include "file_service.h"

#include "../../http/files/file_helpers.h"
#include "../../http/files/file_store.h"
#include "../../infra/storage/storage.h"

#include <cctype>
#include <openssl/rand.h>

namespace
{
const size_t kStoredNamePrefixLength = 24;
const size_t kFilenameMaxLength = 80;

void set_not_found(service_files::ServiceError &error, const char *message);
void set_forbidden(service_files::ServiceError &error, const char *message);

std::string encode_hex(const unsigned char *data, size_t len)
{
    static const char kHexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        hex.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
        hex.push_back(kHexChars[data[i] & 0x0F]);
    }
    return hex;
}

std::string make_share_token()
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    {
        return "";
    }
    return encode_hex(bytes, sizeof(bytes));
}

std::string access_code_hash(const std::string &access_code)
{
    return access_code.empty() ? std::string() : infra_storage::sha256_hex(access_code);
}

void fill_share_result(const repo_mysql::FileShareRecord &record, service_files::ShareResult &result)
{
    result.token = record.token;
    result.file_id = record.file_id;
    result.filename = record.file.original_name;
    result.content_type = record.file.content_type;
    result.size = record.file.file_size;
    result.sha256 = record.file.content_sha256;
    result.has_access_code = !record.access_code_hash.empty();
    result.expires_at = record.expires_at;
    result.max_downloads = record.max_downloads;
    result.download_count = record.download_count;
}

bool validate_share_access(const repo_mysql::FileShareRecord &record, const std::string &access_code,
                           service_files::ServiceError &error)
{
    if (record.file.is_deleted)
    {
        set_not_found(error, "shared file not found");
        return false;
    }
    if (record.is_expired)
    {
        error.status = 410;
        error.title = "Gone";
        error.message = "share link expired";
        return false;
    }
    if (record.max_downloads > 0 && record.download_count >= record.max_downloads)
    {
        error.status = 429;
        error.title = "Too Many Requests";
        error.message = "share download limit reached";
        return false;
    }
    if (!record.access_code_hash.empty())
    {
        if (access_code.empty())
        {
            set_forbidden(error, "access code required");
            return false;
        }
        if (access_code_hash(access_code) != record.access_code_hash)
        {
            set_forbidden(error, "invalid access code");
            return false;
        }
    }
    return true;
}

std::string duplicate_filename_candidate(const std::string &name, int index)
{
    if (index <= 0)
    {
        return name;
    }

    const std::string ext = http_file_helpers::file_extension(name);
    std::string stem = name.substr(0, name.size() - ext.size());
    const std::string suffix = " (" + std::to_string(index) + ")";
    if (stem.size() + suffix.size() + ext.size() > kFilenameMaxLength)
    {
        const size_t max_stem = kFilenameMaxLength - suffix.size() - ext.size();
        stem = stem.substr(0, max_stem);
    }
    return stem + suffix + ext;
}

void set_not_found(service_files::ServiceError &error, const char *message)
{
    error.status = 404;
    error.title = "Not Found";
    error.message = message;
}

void set_forbidden(service_files::ServiceError &error, const char *message)
{
    error.status = 403;
    error.title = "Forbidden";
    error.message = message;
}

void set_conflict(service_files::ServiceError &error, const char *message)
{
    error.status = 409;
    error.title = "Conflict";
    error.message = message;
}

void set_bad_request(service_files::ServiceError &error, const char *message)
{
    error.status = 400;
    error.title = "Bad Request";
    error.message = message;
}

void set_payload_too_large(service_files::ServiceError &error, const std::string &message)
{
    error.status = 413;
    error.title = "Payload Too Large";
    error.message = message;
}

std::string sanitize_folder_name(const std::string &name)
{
    std::string trimmed;
    size_t start = 0;
    while (start < name.size() && std::isspace(static_cast<unsigned char>(name[start])))
    {
        ++start;
    }
    size_t end = name.size();
    while (end > start && std::isspace(static_cast<unsigned char>(name[end - 1])))
    {
        --end;
    }

    for (size_t i = start; i < end && trimmed.size() < 80; ++i)
    {
        const char ch = name[i];
        if (ch == '/' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20)
        {
            continue;
        }
        trimmed.push_back(ch);
    }
    return trimmed;
}
}

namespace service_files
{
bool parse_public_flag(const std::string &value)
{
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
}

std::string json_escape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                escaped += " ";
            }
            else
            {
                escaped += ch;
            }
            break;
        }
    }
    return escaped;
}

std::string build_empty_private_file_list_json(long limit, bool include_deleted)
{
    std::string body = "{\"code\":0,\"files\":[],\"pagination\":{\"limit\":";
    body += std::to_string(limit);
    body += ",\"next_cursor\":0,\"has_more\":false},\"view\":\"";
    body += include_deleted ? "trash" : "active";
    body += "\"}";
    return body;
}

std::string build_empty_public_file_list_json(long limit)
{
    std::string body = "{\"code\":0,\"files\":[],\"pagination\":{\"limit\":";
    body += std::to_string(limit);
    body += ",\"next_cursor\":0,\"has_more\":false}}";
    return body;
}

std::string build_private_file_list_json(const std::vector<repo_mysql::FileListItem> &files,
                                         long next_cursor, int limit, bool include_deleted)
{
    std::string items;
    bool first = true;

    for (const repo_mysql::FileListItem &file : files)
    {
        if (!first)
        {
            items += ",";
        }
        first = false;

        items += "{\"id\":";
        items += std::to_string(file.id);
        items += ",\"filename\":\"";
        items += json_escape(file.filename);
        items += "\",\"content_type\":\"";
        items += json_escape(file.content_type);
        items += "\",\"size\":";
        items += std::to_string(file.size);
        items += ",\"is_public\":";
        items += file.is_public ? "true" : "false";
        items += ",\"folder_id\":";
        items += std::to_string(file.folder_id);
        items += ",\"created_at\":\"";
        items += json_escape(file.created_at);
        items += "\"";
        if (include_deleted)
        {
            items += ",\"deleted_at\":";
            if (file.deleted_at.empty())
            {
                items += "null";
            }
            else
            {
                items += "\"";
                items += json_escape(file.deleted_at);
                items += "\"";
            }
        }
        items += "}";
    }

    std::string body = "{\"code\":0,\"files\":[";
    body += items;
    body += "],\"pagination\":{\"limit\":";
    body += std::to_string(limit);
    body += ",\"next_cursor\":";
    body += std::to_string(next_cursor);
    body += ",\"has_more\":";
    body += next_cursor > 0 ? "true" : "false";
    body += "},\"view\":\"";
    body += include_deleted ? "trash" : "active";
    body += "\"}";
    return body;
}

std::string build_folder_created_json(const repo_mysql::FolderListItem &folder)
{
    std::string body = "{\"code\":0,\"message\":\"folder created\",\"folder\":{\"id\":";
    body += std::to_string(folder.id);
    body += ",\"parent_id\":";
    body += std::to_string(folder.parent_id);
    body += ",\"name\":\"";
    body += json_escape(folder.name);
    body += "\",\"created_at\":\"";
    body += json_escape(folder.created_at);
    body += "\"}}";
    return body;
}

std::string build_drive_items_json(long folder_id, const std::vector<repo_mysql::FolderListItem> &folders,
                                   const std::vector<repo_mysql::FileListItem> &files)
{
    std::string folder_items;
    bool first = true;
    for (const repo_mysql::FolderListItem &folder : folders)
    {
        if (!first)
        {
            folder_items += ",";
        }
        first = false;
        folder_items += "{\"id\":";
        folder_items += std::to_string(folder.id);
        folder_items += ",\"parent_id\":";
        folder_items += std::to_string(folder.parent_id);
        folder_items += ",\"name\":\"";
        folder_items += json_escape(folder.name);
        folder_items += "\",\"created_at\":\"";
        folder_items += json_escape(folder.created_at);
        folder_items += "\"}";
    }

    std::string file_items;
    first = true;
    for (const repo_mysql::FileListItem &file : files)
    {
        if (!first)
        {
            file_items += ",";
        }
        first = false;
        file_items += "{\"id\":";
        file_items += std::to_string(file.id);
        file_items += ",\"folder_id\":";
        file_items += std::to_string(file.folder_id);
        file_items += ",\"filename\":\"";
        file_items += json_escape(file.filename);
        file_items += "\",\"content_type\":\"";
        file_items += json_escape(file.content_type);
        file_items += "\",\"size\":";
        file_items += std::to_string(file.size);
        file_items += ",\"is_public\":";
        file_items += file.is_public ? "true" : "false";
        file_items += ",\"created_at\":\"";
        file_items += json_escape(file.created_at);
        file_items += "\",\"download_url\":\"/api/drive/files/";
        file_items += std::to_string(file.id);
        file_items += "/download\"}";
    }

    std::string body = "{\"code\":0,\"folder_id\":";
    body += std::to_string(folder_id);
    body += ",\"folders\":[";
    body += folder_items;
    body += "],\"files\":[";
    body += file_items;
    body += "]}";
    return body;
}

std::string build_public_file_list_json(const std::vector<repo_mysql::FileListItem> &files,
                                        long next_cursor, int limit)
{
    std::string items;
    bool first = true;

    for (const repo_mysql::FileListItem &file : files)
    {
        if (!first)
        {
            items += ",";
        }
        first = false;

        items += "{\"id\":";
        items += std::to_string(file.id);
        items += ",\"filename\":\"";
        items += json_escape(file.filename);
        items += "\",\"content_type\":\"";
        items += json_escape(file.content_type);
        items += "\",\"size\":";
        items += std::to_string(file.size);
        items += ",\"owner\":\"";
        items += json_escape(file.owner);
        items += "\",\"created_at\":\"";
        items += json_escape(file.created_at);
        items += "\"}";
    }

    std::string body = "{\"code\":0,\"files\":[";
    body += items;
    body += "],\"pagination\":{\"limit\":";
    body += std::to_string(limit);
    body += ",\"next_cursor\":";
    body += std::to_string(next_cursor);
    body += ",\"has_more\":";
    body += next_cursor > 0 ? "true" : "false";
    body += "}}";
    return body;
}

std::string build_public_file_detail_json(const ManagedFileRecord &record, long actual_size)
{
    std::string body = "{\"code\":0,\"file\":{\"id\":";
    body += std::to_string(record.file_id);
    body += ",\"filename\":\"";
    body += json_escape(record.original_name);
    body += "\",\"content_type\":\"";
    body += json_escape(record.content_type);
    body += "\",\"size\":";
    body += std::to_string(actual_size);
    body += ",\"owner\":\"";
    body += json_escape(record.owner);
    body += "\",\"is_public\":true,\"sha256\":\"";
    body += json_escape(record.content_sha256);
    body += "\",\"download_url\":\"/api/files/public/";
    body += std::to_string(record.file_id);
    body += "/download\"}}";
    return body;
}

std::string build_upload_success_json(const UploadResult &result)
{
    std::string body = "{\"code\":0,\"message\":\"upload success\",\"file\":{\"id\":";
    body += std::to_string(result.file_id);
    body += ",\"filename\":\"";
    body += json_escape(result.filename);
    body += "\",\"folder_id\":";
    body += std::to_string(result.folder_id);
    body += ",\"physical_id\":";
    body += std::to_string(result.physical_id);
    body += ",\"size\":";
    body += std::to_string(result.size);
    body += ",\"is_public\":";
    body += result.is_public ? "true" : "false";
    body += ",\"deduplicated\":";
    body += result.deduplicated ? "true" : "false";
    body += ",\"sha256\":\"";
    body += json_escape(result.sha256);
    body += "\"}}";
    return body;
}

std::string build_upload_preflight_json(const UploadQuotaStatus &status)
{
    std::string body = "{\"code\":0,\"allowed\":";
    body += status.allowed ? "true" : "false";
    body += ",\"requested_bytes\":";
    body += std::to_string(status.requested_bytes);
    body += ",\"used_bytes\":";
    body += std::to_string(status.used_bytes);
    body += ",\"remaining_bytes\":";
    body += std::to_string(status.remaining_bytes);
    body += ",\"max_single_file_bytes\":";
    body += std::to_string(status.max_single_file_bytes);
    body += ",\"max_total_bytes\":";
    body += std::to_string(status.max_total_bytes);
    body += ",\"reason\":";
    if (status.reason.empty())
    {
        body += "null";
    }
    else
    {
        body += "\"";
        body += json_escape(status.reason);
        body += "\"";
    }
    body += "}";
    return body;
}

std::string build_restore_success_json(long file_id, const std::string &filename)
{
    std::string body = "{\"code\":0,\"message\":\"file restored\",\"file\":{\"id\":";
    body += std::to_string(file_id);
    body += ",\"filename\":\"";
    body += json_escape(filename);
    body += "\"}}";
    return body;
}

std::string build_share_json(const ShareResult &share)
{
    std::string body = "{\"code\":0,\"share\":{\"token\":\"";
    body += json_escape(share.token);
    body += "\",\"file_id\":";
    body += std::to_string(share.file_id);
    body += ",\"filename\":\"";
    body += json_escape(share.filename);
    body += "\",\"content_type\":\"";
    body += json_escape(share.content_type);
    body += "\",\"size\":";
    body += std::to_string(share.size);
    body += ",\"sha256\":\"";
    body += json_escape(share.sha256);
    body += "\",\"has_access_code\":";
    body += share.has_access_code ? "true" : "false";
    body += ",\"expires_at\":";
    if (share.expires_at.empty())
    {
        body += "null";
    }
    else
    {
        body += "\"";
        body += json_escape(share.expires_at);
        body += "\"";
    }
    body += ",\"max_downloads\":";
    body += std::to_string(share.max_downloads);
    body += ",\"download_count\":";
    body += std::to_string(share.download_count);
    body += ",\"share_url\":\"/share?token=";
    body += json_escape(share.token);
    body += "\",\"detail_url\":\"/api/share/";
    body += json_escape(share.token);
    body += "\",\"download_url\":\"/api/share/";
    body += json_escape(share.token);
    body += "/download\"}}";
    return body;
}

bool load_owned_file_record(MYSQL *mysql, const std::string &owner, long file_id,
                            ManagedFileRecord &record, ServiceError &error, bool include_deleted)
{
    if (!http_file_store::fetch_file_record(mysql, file_id, record, include_deleted))
    {
        set_not_found(error, "file not found");
        return false;
    }

    if (record.owner != owner)
    {
        set_forbidden(error, "forbidden");
        return false;
    }

    return true;
}

bool list_private_files(MYSQL *mysql, const std::string &owner, bool include_deleted,
                        long cursor, long limit, std::string &body)
{
    repo_mysql::PageIds page;
    if (!repo_mysql::fetch_private_file_page_ids(mysql, owner, include_deleted, cursor, limit, page))
    {
        return false;
    }

    if (page.ids.empty())
    {
        body = build_empty_private_file_list_json(limit, include_deleted);
        return true;
    }

    std::vector<repo_mysql::FileListItem> files;
    if (!repo_mysql::fetch_private_file_list(mysql, page.ids, files))
    {
        return false;
    }

    body = build_private_file_list_json(files, page.next_cursor, static_cast<int>(limit), include_deleted);
    return true;
}

bool list_public_files(MYSQL *mysql, long cursor, long limit, std::string &body)
{
    repo_mysql::PageIds page;
    if (!repo_mysql::fetch_public_file_page_ids(mysql, cursor, limit, page))
    {
        return false;
    }

    if (page.ids.empty())
    {
        body = build_empty_public_file_list_json(limit);
        return true;
    }

    std::vector<repo_mysql::FileListItem> files;
    if (!repo_mysql::fetch_public_file_list(mysql, page.ids, files))
    {
        return false;
    }

    body = build_public_file_list_json(files, page.next_cursor, static_cast<int>(limit));
    return true;
}

bool load_public_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, ServiceError &error)
{
    if (!http_file_store::fetch_file_record(mysql, file_id, record))
    {
        set_not_found(error, "file not found");
        return false;
    }
    if (!record.is_public)
    {
        set_forbidden(error, "file is private");
        return false;
    }
    return true;
}

bool load_public_file_detail(MYSQL *mysql, const std::string &doc_root, long file_id,
                             ManagedFileRecord &record, long &actual_size, ServiceError &error)
{
    if (!load_public_file_record(mysql, file_id, record, error))
    {
        return false;
    }

    const std::string disk_path = infra_storage::file_path(doc_root, record.stored_name);
    if (!infra_storage::file_exists(disk_path))
    {
        set_not_found(error, "file content not found");
        return false;
    }

    actual_size = record.file_size;
    infra_storage::file_size(disk_path, actual_size);
    return true;
}

bool inspect_upload_quota(MYSQL *mysql, const std::string &owner, long requested_bytes,
                          const UploadQuota &quota, UploadQuotaStatus &status, ServiceError &error)
{
    status = UploadQuotaStatus();
    status.requested_bytes = requested_bytes;
    status.max_single_file_bytes = quota.max_single_file_bytes;
    status.max_total_bytes = quota.max_total_bytes;

    if (requested_bytes < 0)
    {
        set_bad_request(error, "invalid file size");
        status.reason = error.message;
        return false;
    }

    if (!repo_mysql::fetch_user_storage_usage(mysql, owner, status.used_bytes))
    {
        return false;
    }

    status.remaining_bytes = quota.max_total_bytes > 0 && status.used_bytes < quota.max_total_bytes
                                 ? quota.max_total_bytes - status.used_bytes
                                 : 0;
    if (quota.max_total_bytes <= 0)
    {
        status.remaining_bytes = 0;
    }

    if (quota.max_single_file_bytes > 0 && requested_bytes > quota.max_single_file_bytes)
    {
        status.reason = "file exceeds single file limit of " + std::to_string(quota.max_single_file_bytes) + " bytes";
        set_payload_too_large(error, status.reason);
        return true;
    }

    if (quota.max_total_bytes > 0 && requested_bytes > status.remaining_bytes)
    {
        status.reason = "user storage quota exceeded";
        set_conflict(error, status.reason.c_str());
        return true;
    }

    status.allowed = true;
    return true;
}

std::string ensure_unique_owned_filename(MYSQL *mysql, const std::string &owner,
                                         const std::string &requested_name, long folder_id, long ignore_file_id)
{
    std::string sanitized = http_file_helpers::sanitize_filename(requested_name);
    if (sanitized.empty())
    {
        sanitized = "file.txt";
    }

    for (int index = 0; index < 1000; ++index)
    {
        const std::string candidate = duplicate_filename_candidate(sanitized, index);
        bool exists = false;
        if (!repo_mysql::original_name_exists(mysql, owner, candidate, folder_id, ignore_file_id, exists))
        {
            return candidate;
        }
        if (!exists)
        {
            return candidate;
        }
    }

    return duplicate_filename_candidate(sanitized, 1000);
}

bool create_folder(MYSQL *mysql, const std::string &owner, long parent_id, const std::string &name,
                   repo_mysql::FolderListItem &folder, ServiceError &error)
{
    const std::string safe_name = sanitize_folder_name(name);
    if (safe_name.empty())
    {
        set_bad_request(error, "folder name is required");
        return false;
    }
    if (parent_id < 0)
    {
        set_bad_request(error, "invalid parent_id");
        return false;
    }

    bool exists = false;
    if (!repo_mysql::folder_exists(mysql, owner, parent_id, exists))
    {
        return false;
    }
    if (!exists)
    {
        set_not_found(error, "parent folder not found");
        return false;
    }

    bool name_exists = false;
    if (!repo_mysql::folder_name_exists(mysql, owner, parent_id, safe_name, name_exists))
    {
        return false;
    }
    if (name_exists)
    {
        set_conflict(error, "folder name already exists");
        return false;
    }

    repo_mysql::FolderCreateRecord record;
    record.owner = owner;
    record.parent_id = parent_id;
    record.name = safe_name;

    long folder_id = 0;
    if (!repo_mysql::insert_folder(mysql, record, folder_id))
    {
        if (repo_mysql::last_errno(mysql) == 1062)
        {
            set_conflict(error, "folder name already exists");
        }
        return false;
    }

    if (repo_mysql::fetch_folder(mysql, owner, folder_id, folder))
    {
        return true;
    }

    folder.id = folder_id;
    folder.parent_id = parent_id;
    folder.name = safe_name;
    folder.created_at = "";
    return true;
}

bool delete_empty_folder(MYSQL *mysql, const std::string &owner, long folder_id,
                         repo_mysql::FolderListItem &folder, ServiceError &error)
{
    if (folder_id <= 0)
    {
        set_bad_request(error, "root folder cannot be deleted");
        return false;
    }

    if (!repo_mysql::fetch_folder(mysql, owner, folder_id, folder))
    {
        set_not_found(error, "folder not found");
        return false;
    }

    bool has_children = false;
    if (!repo_mysql::folder_has_active_children(mysql, owner, folder_id, has_children))
    {
        return false;
    }
    if (has_children)
    {
        set_conflict(error, "folder is not empty");
        return false;
    }

    return repo_mysql::soft_delete_folder(mysql, owner, folder_id);
}

bool list_drive_items(MYSQL *mysql, const std::string &owner, long folder_id, std::string &body,
                      ServiceError &error)
{
    if (folder_id < 0)
    {
        set_bad_request(error, "invalid folder_id");
        return false;
    }

    bool exists = false;
    if (!repo_mysql::folder_exists(mysql, owner, folder_id, exists))
    {
        return false;
    }
    if (!exists)
    {
        set_not_found(error, "folder not found");
        return false;
    }

    std::vector<repo_mysql::FolderListItem> folders;
    std::vector<repo_mysql::FileListItem> files;
    if (!repo_mysql::fetch_drive_folders(mysql, owner, folder_id, folders) ||
        !repo_mysql::fetch_drive_files(mysql, owner, folder_id, files))
    {
        return false;
    }

    body = build_drive_items_json(folder_id, folders, files);
    return true;
}

bool create_uploaded_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner,
                          const std::string &stored_name_prefix, UploadPayload &payload,
                          bool is_public, const UploadQuota &quota, UploadResult &result, ServiceError *error)
{
    ServiceError quota_error;
    UploadQuotaStatus quota_status;
    if (!inspect_upload_quota(mysql, owner, payload.size, quota, quota_status, quota_error))
    {
        return false;
    }
    if (!quota_status.allowed)
    {
        if (error != nullptr)
        {
            *error = quota_error;
        }
        return false;
    }

    bool folder_exists = false;
    if (!repo_mysql::folder_exists(mysql, owner, payload.folder_id, folder_exists))
    {
        return false;
    }
    if (!folder_exists)
    {
        if (error != nullptr)
        {
            set_not_found(*error, "folder not found");
        }
        return false;
    }

    if (!infra_storage::ensure_directory(infra_storage::storage_root(doc_root)))
    {
        return false;
    }

    payload.filename = ensure_unique_owned_filename(mysql, owner, payload.filename, payload.folder_id);
    if (payload.sha256.empty() && !payload.content.empty())
    {
        payload.sha256 = infra_storage::sha256_hex(payload.content);
    }

    repo_mysql::PhysicalFileRecord physical;
    bool deduplicated = false;
    const bool has_physical = !payload.sha256.empty() &&
                              repo_mysql::fetch_physical_file_by_sha256(mysql, payload.sha256, physical);
    if (has_physical)
    {
        const std::string disk_path = infra_storage::file_path(doc_root, physical.stored_name);
        if (infra_storage::file_exists(disk_path))
        {
            deduplicated = true;
            if (payload.use_temp_file)
            {
                infra_storage::remove_file(payload.temp_path);
            }
        }
        else if (payload.use_temp_file)
        {
            if (!infra_storage::move_file_or_copy(payload.temp_path, disk_path))
            {
                return false;
            }
        }
        else if (!infra_storage::write_file(disk_path, payload.content))
        {
            return false;
        }

        if (!repo_mysql::increment_physical_ref(mysql, physical.id))
        {
            return false;
        }
    }
    else
    {
        physical.stored_name = stored_name_prefix.substr(0, kStoredNamePrefixLength);
        if (!payload.sha256.empty())
        {
            physical.stored_name += "_" + payload.sha256.substr(0, 12);
        }
        physical.stored_name += http_file_helpers::file_extension(payload.filename);
        physical.sha256 = payload.sha256;
        physical.file_size = payload.size;
        physical.ref_count = 1;

        const std::string disk_path = infra_storage::file_path(doc_root, physical.stored_name);
        if (payload.use_temp_file)
        {
            if (!infra_storage::move_file_or_copy(payload.temp_path, disk_path))
            {
                return false;
            }
        }
        else if (!infra_storage::write_file(disk_path, payload.content))
        {
            return false;
        }

        long physical_id = 0;
        if (!repo_mysql::insert_physical_file(mysql, physical, physical_id))
        {
            if (repo_mysql::last_errno(mysql) == 1062 && repo_mysql::fetch_physical_file_by_sha256(mysql, payload.sha256, physical))
            {
                const std::string existing_path = infra_storage::file_path(doc_root, physical.stored_name);
                if (!infra_storage::file_exists(existing_path) &&
                    !infra_storage::move_file_or_copy(disk_path, existing_path))
                {
                    infra_storage::remove_file(disk_path);
                    return false;
                }
                else if (infra_storage::file_exists(disk_path))
                {
                    infra_storage::remove_file(disk_path);
                }
                deduplicated = true;
                if (!repo_mysql::increment_physical_ref(mysql, physical.id))
                {
                    return false;
                }
            }
            else
            {
                infra_storage::remove_file(disk_path);
                return false;
            }
        }
        else
        {
            physical.id = physical_id;
        }
    }

    repo_mysql::FileCreateRecord file_record;
    file_record.owner = owner;
    file_record.stored_name = physical.stored_name;
    file_record.physical_id = physical.id;
    file_record.original_name = payload.filename;
    file_record.content_type = payload.content_type;
    file_record.folder_id = payload.folder_id;
    file_record.file_size = payload.size;
    file_record.is_public = is_public;
    file_record.sha256 = payload.sha256;

    long file_id = 0;
    if (!repo_mysql::insert_file(mysql, file_record, file_id))
    {
        repo_mysql::decrement_physical_ref(mysql, physical.id);
        return false;
    }

    result.file_id = file_id;
    result.folder_id = payload.folder_id;
    result.physical_id = physical.id;
    result.filename = payload.filename;
    result.size = payload.size;
    result.is_public = is_public;
    result.deduplicated = deduplicated;
    result.sha256 = payload.sha256;
    return true;
}

bool soft_delete_file(MYSQL *mysql, const std::string &owner, long file_id,
                      ManagedFileRecord &record, ServiceError &error)
{
    if (!load_owned_file_record(mysql, owner, file_id, record, error))
    {
        return false;
    }
    if (!repo_mysql::soft_delete_file(mysql, file_id))
    {
        return false;
    }
    if (record.physical_id > 0)
    {
        repo_mysql::decrement_physical_ref(mysql, record.physical_id);
    }
    return true;
}

bool update_file_visibility(MYSQL *mysql, const std::string &owner, long file_id, bool is_public,
                            ManagedFileRecord &record, ServiceError &error)
{
    if (!load_owned_file_record(mysql, owner, file_id, record, error))
    {
        return false;
    }
    return repo_mysql::update_file_visibility(mysql, file_id, is_public);
}

bool restore_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner, long file_id,
                  ManagedFileRecord &record, std::string &restored_name, ServiceError &error)
{
    if (!load_owned_file_record(mysql, owner, file_id, record, error, true))
    {
        return false;
    }
    if (!record.is_deleted)
    {
        set_conflict(error, "file is not in recycle bin");
        return false;
    }
    if (!infra_storage::file_exists(infra_storage::file_path(doc_root, record.stored_name)))
    {
        set_not_found(error, "file content not found");
        return false;
    }

    restored_name = ensure_unique_owned_filename(mysql, owner, record.original_name, record.folder_id, file_id);
    if (!repo_mysql::restore_file(mysql, file_id, restored_name))
    {
        return false;
    }
    if (record.physical_id > 0)
    {
        repo_mysql::increment_physical_ref(mysql, record.physical_id);
    }
    return true;
}
bool create_share_link(MYSQL *mysql, const std::string &owner, long file_id, const ShareOptions &options,
                       ShareResult &result, ServiceError &error)
{
    if (options.expires_in_seconds < 0 || options.max_downloads < 0)
    {
        set_bad_request(error, "invalid share limits");
        return false;
    }
    if (options.access_code.size() > 32)
    {
        set_bad_request(error, "access code is too long");
        return false;
    }

    ManagedFileRecord file;
    if (!load_owned_file_record(mysql, owner, file_id, file, error))
    {
        return false;
    }

    repo_mysql::FileShareCreateRecord share;
    share.file_id = file_id;
    share.owner = owner;
    share.access_code_hash = access_code_hash(options.access_code);
    share.expires_in_seconds = options.expires_in_seconds;
    share.max_downloads = options.max_downloads;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        share.token = make_share_token();
        if (share.token.empty())
        {
            return false;
        }
        if (repo_mysql::insert_file_share(mysql, share))
        {
            repo_mysql::FileShareRecord stored;
            if (!repo_mysql::fetch_file_share(mysql, share.token, stored))
            {
                return false;
            }
            fill_share_result(stored, result);
            return true;
        }
    }
    return false;
}

bool load_share_detail(MYSQL *mysql, const std::string &token, const std::string &access_code,
                       ShareResult &result, ServiceError &error)
{
    repo_mysql::FileShareRecord share;
    if (!repo_mysql::fetch_file_share(mysql, token, share))
    {
        set_not_found(error, "share link not found");
        return false;
    }
    if (!validate_share_access(share, access_code, error))
    {
        return false;
    }

    fill_share_result(share, result);
    return true;
}

bool load_share_download(MYSQL *mysql, const std::string &token, const std::string &access_code,
                         ManagedFileRecord &record, ShareResult &result, ServiceError &error)
{
    repo_mysql::FileShareRecord share;
    if (!repo_mysql::fetch_file_share(mysql, token, share))
    {
        set_not_found(error, "share link not found");
        return false;
    }
    if (!validate_share_access(share, access_code, error))
    {
        return false;
    }
    if (!repo_mysql::increment_file_share_download_count(mysql, token))
    {
        error.status = 429;
        error.title = "Too Many Requests";
        error.message = "share download limit reached";
        return false;
    }

    share.download_count += 1;
    record = share.file;
    fill_share_result(share, result);
    return true;
}

}
