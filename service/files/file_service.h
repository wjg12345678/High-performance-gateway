#ifndef SERVICE_FILES_FILE_SERVICE_H
#define SERVICE_FILES_FILE_SERVICE_H

#include <mysql/mysql.h>
#include <string>
#include <vector>

#include "../../http/files/file_types.h"
#include "../../repo/mysql/file_repository.h"

namespace service_files
{
struct EmptyTrashResult
{
    long deleted_count;

    EmptyTrashResult() : deleted_count(0) {}
};

enum class ErrorCode
{
    None,
    InvalidArgument,
    Forbidden,
    NotFound,
    Conflict,
    Gone,
    TooManyRequests,
    PayloadTooLarge,
    Internal
};

struct ServiceError
{
    ErrorCode code;
    std::string message;

    ServiceError() : code(ErrorCode::Internal) {}
};

bool parse_public_flag(const std::string &value);
std::string json_escape(const std::string &value);

std::string build_empty_public_file_list_json(long limit);
std::string build_public_file_list_json(const std::vector<repo_mysql::FileListItem> &files,
                                        long next_cursor, int limit);
std::string build_public_file_detail_json(const ManagedFileRecord &record, long actual_size);
std::string build_folder_created_json(const repo_mysql::FolderListItem &folder);
std::string build_drive_items_json(long folder_id, const std::vector<repo_mysql::FolderListItem> &folders,
                                   const std::vector<repo_mysql::FileListItem> &files);
std::string build_trash_items_json(const std::vector<repo_mysql::FileListItem> &files);
std::string build_file_restored_json(const ManagedFileRecord &record);

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
bool list_trash_items(MYSQL *mysql, const std::string &owner, std::string &body);

bool list_public_files(MYSQL *mysql, long cursor, long limit, std::string &body);
bool load_public_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, ServiceError &error);
bool load_public_file_detail(MYSQL *mysql, const std::string &doc_root, long file_id,
                             ManagedFileRecord &record, long &actual_size, ServiceError &error);
bool soft_delete_file(MYSQL *mysql, const std::string &owner, long file_id,
                      ManagedFileRecord &record, ServiceError &error);
bool restore_file(MYSQL *mysql, const std::string &owner, long file_id,
                  ManagedFileRecord &record, ServiceError &error);
bool validate_hard_delete_candidate(const ManagedFileRecord &record, const std::string &owner,
                                    ServiceError &error);
bool hard_delete_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner, long file_id,
                      ManagedFileRecord &record, ServiceError &error);
bool empty_trash(MYSQL *mysql, const std::string &doc_root, const std::string &owner,
                 EmptyTrashResult &result, ServiceError &error);
bool update_file_visibility(MYSQL *mysql, const std::string &owner, long file_id, bool is_public,
                            ManagedFileRecord &record, ServiceError &error);
}

#endif
