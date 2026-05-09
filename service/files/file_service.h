#ifndef SERVICE_FILES_FILE_SERVICE_H
#define SERVICE_FILES_FILE_SERVICE_H

#include <mysql/mysql.h>
#include <string>
#include <vector>

#include "../../http/files/file_types.h"
#include "../../repo/mysql/file_repository.h"

namespace service_files
{
struct UploadPayload
{
    std::string filename;
    std::string content;
    std::string content_type;
    std::string temp_path;
    std::string sha256;
    long folder_id;
    long size;
    bool use_temp_file;

    UploadPayload() : folder_id(0), size(0), use_temp_file(false) {}
};

struct UploadResult
{
    long file_id;
    long folder_id;
    long physical_id;
    std::string filename;
    long size;
    bool is_public;
    bool deduplicated;
    std::string sha256;

    UploadResult() : file_id(0), folder_id(0), physical_id(0), size(0), is_public(false), deduplicated(false) {}
};

struct UploadQuota
{
    long max_single_file_bytes;
    long max_total_bytes;

    UploadQuota() : max_single_file_bytes(0), max_total_bytes(0) {}
};

struct UploadQuotaStatus
{
    long requested_bytes;
    long used_bytes;
    long remaining_bytes;
    long max_single_file_bytes;
    long max_total_bytes;
    bool allowed;
    std::string reason;

    UploadQuotaStatus()
        : requested_bytes(0), used_bytes(0), remaining_bytes(0), max_single_file_bytes(0),
          max_total_bytes(0), allowed(false) {}
};


struct ShareOptions
{
    std::string access_code;
    long expires_in_seconds;
    long max_downloads;

    ShareOptions() : expires_in_seconds(0), max_downloads(0) {}
};

struct ShareResult
{
    std::string token;
    long file_id;
    std::string filename;
    std::string content_type;
    long size;
    std::string sha256;
    bool has_access_code;
    std::string expires_at;
    long max_downloads;
    long download_count;

    ShareResult() : file_id(0), size(0), has_access_code(false), max_downloads(0), download_count(0) {}
};

struct ServiceError
{
    int status;
    const char *title;
    std::string message;

    ServiceError() : status(500), title("Internal Error") {}
};

bool parse_public_flag(const std::string &value);
std::string json_escape(const std::string &value);

std::string build_empty_private_file_list_json(long limit, bool include_deleted);
std::string build_empty_public_file_list_json(long limit);
std::string build_private_file_list_json(const std::vector<repo_mysql::FileListItem> &files,
                                         long next_cursor, int limit, bool include_deleted);
std::string build_public_file_list_json(const std::vector<repo_mysql::FileListItem> &files,
                                        long next_cursor, int limit);
std::string build_public_file_detail_json(const ManagedFileRecord &record, long actual_size);
std::string build_upload_success_json(const UploadResult &result);
std::string build_upload_preflight_json(const UploadQuotaStatus &status);
std::string build_restore_success_json(long file_id, const std::string &filename);
std::string build_share_json(const ShareResult &share);
std::string build_folder_created_json(const repo_mysql::FolderListItem &folder);
std::string build_drive_items_json(long folder_id, const std::vector<repo_mysql::FolderListItem> &folders,
                                   const std::vector<repo_mysql::FileListItem> &files);

bool load_owned_file_record(MYSQL *mysql, const std::string &owner, long file_id,
                            ManagedFileRecord &record, ServiceError &error,
                            bool include_deleted = false);
std::string ensure_unique_owned_filename(MYSQL *mysql, const std::string &owner,
                                         const std::string &requested_name, long folder_id,
                                         long ignore_file_id = 0);
bool create_folder(MYSQL *mysql, const std::string &owner, long parent_id, const std::string &name,
                   repo_mysql::FolderListItem &folder, ServiceError &error);
bool delete_empty_folder(MYSQL *mysql, const std::string &owner, long folder_id,
                         repo_mysql::FolderListItem &folder, ServiceError &error);
bool list_drive_items(MYSQL *mysql, const std::string &owner, long folder_id, std::string &body,
                      ServiceError &error);

bool list_private_files(MYSQL *mysql, const std::string &owner, bool include_deleted,
                        long cursor, long limit, std::string &body);
bool list_public_files(MYSQL *mysql, long cursor, long limit, std::string &body);
bool load_public_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, ServiceError &error);
bool load_public_file_detail(MYSQL *mysql, const std::string &doc_root, long file_id,
                             ManagedFileRecord &record, long &actual_size, ServiceError &error);
bool inspect_upload_quota(MYSQL *mysql, const std::string &owner, long requested_bytes,
                          const UploadQuota &quota, UploadQuotaStatus &status, ServiceError &error);
bool create_uploaded_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner,
                          const std::string &stored_name_prefix, UploadPayload &payload,
                          bool is_public, const UploadQuota &quota, UploadResult &result,
                          ServiceError *error = nullptr);
bool soft_delete_file(MYSQL *mysql, const std::string &owner, long file_id,
                      ManagedFileRecord &record, ServiceError &error);
bool update_file_visibility(MYSQL *mysql, const std::string &owner, long file_id, bool is_public,
                            ManagedFileRecord &record, ServiceError &error);
bool restore_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner, long file_id,
                  ManagedFileRecord &record, std::string &restored_name, ServiceError &error);
bool create_share_link(MYSQL *mysql, const std::string &owner, long file_id, const ShareOptions &options,
                       ShareResult &result, ServiceError &error);
bool load_share_detail(MYSQL *mysql, const std::string &token, const std::string &access_code,
                       ShareResult &result, ServiceError &error);
bool load_share_download(MYSQL *mysql, const std::string &token, const std::string &access_code,
                         ManagedFileRecord &record, ShareResult &result, ServiceError &error);
}

#endif
