#include "file_service.h"

#include "../../http/files/file_helpers.h"
#include "../../http/files/file_store.h"
#include "../../infra/storage/storage.h"
#include "../../repo/mysql/mysql_utils.h"

#include <cctype>
#include <cstdlib>

namespace
{
const size_t kFilenameMaxLength = 80;

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
    error.code = service_files::ErrorCode::NotFound;
    error.message = message;
}

void set_forbidden(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::Forbidden;
    error.message = message;
}

void set_conflict(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::Conflict;
    error.message = message;
}

void set_bad_request(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::InvalidArgument;
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

class MysqlTransaction
{
public:
    explicit MysqlTransaction(MYSQL *mysql) : m_mysql(mysql), m_active(false), m_committed(false) {}

    ~MysqlTransaction()
    {
        if (m_active && !m_committed)
        {
            repo_mysql::rollback_transaction(m_mysql);
        }
    }

    bool begin()
    {
        if (!repo_mysql::begin_transaction(m_mysql))
        {
            return false;
        }
        m_active = true;
        return true;
    }

    bool commit()
    {
        if (!m_active)
        {
            return false;
        }
        if (!repo_mysql::commit_transaction(m_mysql))
        {
            return false;
        }
        m_committed = true;
        m_active = false;
        return true;
    }

private:
    MYSQL *m_mysql;
    bool m_active;
    bool m_committed;
};
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

std::string build_empty_public_file_list_json(long limit)
{
    std::string body = "{\"code\":0,\"files\":[],\"pagination\":{\"limit\":";
    body += std::to_string(limit);
    body += ",\"next_cursor\":0,\"has_more\":false}}";
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

std::string build_trash_items_json(const std::vector<repo_mysql::FileListItem> &files)
{
    std::string file_items;
    bool first = true;
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
        file_items += ",\"created_at\":\"";
        file_items += json_escape(file.created_at);
        file_items += "\",\"deleted_at\":\"";
        file_items += json_escape(file.deleted_at);
        file_items += "\"}";
    }

    std::string body = "{\"code\":0,\"files\":[";
    body += file_items;
    body += "]}";
    return body;
}

std::string build_file_restored_json(const ManagedFileRecord &record)
{
    std::string body = "{\"code\":0,\"message\":\"file restored\",\"file\":{\"id\":";
    body += std::to_string(record.file_id);
    body += ",\"folder_id\":";
    body += std::to_string(record.folder_id);
    body += ",\"filename\":\"";
    body += json_escape(record.original_name);
    body += "\",\"content_type\":\"";
    body += json_escape(record.content_type);
    body += "\",\"size\":";
    body += std::to_string(record.file_size);
    body += ",\"is_public\":";
    body += record.is_public ? "true" : "false";
    body += "}}";
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

bool list_trash_items(MYSQL *mysql, const std::string &owner, std::string &body)
{
    std::vector<repo_mysql::FileListItem> files;
    if (!repo_mysql::fetch_trash_files(mysql, owner, files))
    {
        return false;
    }

    body = build_trash_items_json(files);
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
    return true;
}

bool restore_file(MYSQL *mysql, const std::string &owner, long file_id,
                  ManagedFileRecord &record, ServiceError &error)
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

    bool folder_exists = false;
    if (!repo_mysql::folder_exists(mysql, owner, record.folder_id, folder_exists))
    {
        return false;
    }
    const long restore_folder_id = folder_exists ? record.folder_id : 0;
    const std::string restored_name = ensure_unique_owned_filename(mysql, owner, record.original_name,
                                                                   restore_folder_id, record.file_id);
    if (!repo_mysql::restore_file(mysql, owner, file_id, restore_folder_id, restored_name))
    {
        return false;
    }

    record.folder_id = restore_folder_id;
    record.original_name = restored_name;
    record.is_public = false;
    record.is_deleted = false;
    record.deleted_at.clear();
    return true;
}

bool validate_hard_delete_candidate(const ManagedFileRecord &record, const std::string &owner,
                                    ServiceError &error)
{
    if (record.owner != owner)
    {
        set_forbidden(error, "forbidden");
        return false;
    }
    if (!record.is_deleted)
    {
        set_conflict(error, "file must be moved to recycle bin before permanent deletion");
        return false;
    }
    return true;
}

bool hard_delete_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner, long file_id,
                      ManagedFileRecord &record, ServiceError &error)
{
    MysqlTransaction transaction(mysql);
    if (!transaction.begin())
    {
        return false;
    }

    if (!repo_mysql::fetch_file_record_for_update(mysql, file_id, record, true))
    {
        set_not_found(error, "file not found");
        return false;
    }
    if (!validate_hard_delete_candidate(record, owner, error))
    {
        return false;
    }

    const long physical_id = record.physical_id;
    const std::string stored_name = record.stored_name;
    if (!repo_mysql::hard_delete_file(mysql, owner, file_id))
    {
        return false;
    }

    bool physical_deleted = false;
    if (physical_id > 0 && !repo_mysql::delete_physical_file_if_unreferenced(mysql, physical_id, physical_deleted))
    {
        return false;
    }

    if (!transaction.commit())
    {
        return false;
    }
    if (physical_deleted && !stored_name.empty())
    {
        infra_storage::remove_file(infra_storage::file_path(doc_root, stored_name));
    }
    return true;
}

bool empty_trash(MYSQL *mysql, const std::string &doc_root, const std::string &owner,
                 EmptyTrashResult &result, ServiceError &error)
{
    result.deleted_count = 0;

    std::vector<long> file_ids;
    if (!repo_mysql::fetch_trash_file_ids(mysql, owner, file_ids))
    {
        return false;
    }

    for (long file_id : file_ids)
    {
        ManagedFileRecord record;
        if (!hard_delete_file(mysql, doc_root, owner, file_id, record, error))
        {
            return false;
        }
        ++result.deleted_count;
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

}
