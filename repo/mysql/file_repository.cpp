#include "file_repository.h"

#include "mysql_utils.h"

#include <cstdlib>
#include <cstdio>

namespace repo_mysql
{
namespace
{
bool fetch_page_ids(MYSQL *mysql, const std::string &sql, long limit, PageIds &page)
{
    if (mysql == nullptr)
    {
        return false;
    }

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    page.ids.clear();
    page.next_cursor = 0;
    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        const long id = (row[0] != nullptr) ? atol(row[0]) : 0;
        if (static_cast<long>(page.ids.size()) < limit)
        {
            page.ids.push_back(id);
        }
        else
        {
            page.next_cursor = id;
            break;
        }
    }

    mysql_free_result(result);
    return true;
}

bool fill_file_record_from_row(MYSQL_ROW row, long file_id, ManagedFileRecord &record)
{
    if (row == nullptr)
    {
        return false;
    }

    record.file_id = file_id;
    record.owner = row[0] ? row[0] : "";
    record.stored_name = row[1] ? row[1] : "";
    record.original_name = row[2] ? row[2] : "";
    record.content_type = row[3] ? row[3] : "application/octet-stream";
    record.file_size = row[4] ? atol(row[4]) : 0;
    record.is_public = row[5] ? atoi(row[5]) != 0 : false;
    record.content_sha256 = row[6] ? row[6] : "";
    record.deleted_at = row[7] ? row[7] : "";
    record.folder_id = row[8] ? atol(row[8]) : 0;
    record.physical_id = row[9] ? atol(row[9]) : 0;
    record.is_deleted = !record.deleted_at.empty();
    return true;
}

bool fetch_file_record_impl(MYSQL *mysql, long file_id, ManagedFileRecord &record,
                            bool include_deleted, bool for_update)
{
    if (mysql == nullptr)
    {
        return false;
    }

    char query[1100];
    snprintf(query, sizeof(query),
             "SELECT u.username, COALESCE(NULLIF(p.stored_name, ''), f.stored_name), f.original_name, f.content_type, f.file_size, f.is_public, "
             "COALESCE(f.content_sha256, ''), COALESCE(DATE_FORMAT(f.deleted_at, '%%Y-%%m-%%d %%H:%%i:%%s'), ''), "
             "COALESCE(f.folder_id, 0), f.physical_id "
             "FROM files f JOIN users u ON u.id=f.user_id "
             "JOIN physical_files p ON f.physical_id=p.id WHERE f.id=%ld%s LIMIT 1%s",
             file_id,
             include_deleted ? "" : " AND f.deleted_at IS NULL",
             for_update ? " FOR UPDATE" : "");
    if (mysql_query(mysql, query) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    const bool found = fill_file_record_from_row(row, file_id, record);
    mysql_free_result(result);
    return found;
}

bool fill_physical_record_from_row(MYSQL_ROW row, PhysicalFileRecord &record)
{
    if (row == nullptr)
    {
        return false;
    }

    record.id = row[0] ? atol(row[0]) : 0;
    record.sha256 = row[1] ? row[1] : "";
    record.stored_name = row[2] ? row[2] : "";
    record.file_size = row[3] ? atol(row[3]) : 0;
    record.ref_count = row[4] ? atol(row[4]) : 0;
    return record.id > 0 && !record.stored_name.empty();
}

bool fetch_physical_file_by_sha256_impl(MYSQL *mysql, const std::string &sha256,
                                        PhysicalFileRecord &record, bool for_update)
{
    if (mysql == nullptr || sha256.empty())
    {
        return false;
    }

    std::string sql = "SELECT id, sha256, stored_name, file_size, ref_count FROM physical_files WHERE sha256='" +
                      escape(mysql, sha256) + "' LIMIT 1";
    if (for_update)
    {
        sql += " FOR UPDATE";
    }
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    const bool found = fill_physical_record_from_row(row, record);
    mysql_free_result(result);
    return found;
}
}

bool fetch_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, bool include_deleted)
{
    return fetch_file_record_impl(mysql, file_id, record, include_deleted, false);
}

bool fetch_file_record_for_update(MYSQL *mysql, long file_id, ManagedFileRecord &record, bool include_deleted)
{
    return fetch_file_record_impl(mysql, file_id, record, include_deleted, true);
}

unsigned int last_errno(MYSQL *mysql)
{
    return mysql == nullptr ? 0 : mysql_errno(mysql);
}

bool lock_user_for_update(MYSQL *mysql, const std::string &owner)
{
    if (mysql == nullptr || owner.empty())
    {
        return false;
    }

    const std::string sql = "SELECT id FROM users WHERE username='" + escape(mysql, owner) +
                            "' AND disabled_at IS NULL LIMIT 1 FOR UPDATE";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    const bool found = mysql_fetch_row(result) != nullptr;
    mysql_free_result(result);
    return found;
}

bool fetch_physical_file_by_sha256(MYSQL *mysql, const std::string &sha256, PhysicalFileRecord &record)
{
    return fetch_physical_file_by_sha256_impl(mysql, sha256, record, false);
}

bool fetch_physical_file_by_sha256_for_update(MYSQL *mysql, const std::string &sha256,
                                              PhysicalFileRecord &record)
{
    return fetch_physical_file_by_sha256_impl(mysql, sha256, record, true);
}

bool insert_physical_file(MYSQL *mysql, const PhysicalFileRecord &record, long &physical_id)
{
    if (mysql == nullptr || record.sha256.empty() || record.stored_name.empty())
    {
        return false;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO physical_files(sha256, stored_name, file_size, ref_count) VALUES('%s', '%s', %ld, 0)",
             escape(mysql, record.sha256).c_str(),
             escape(mysql, record.stored_name).c_str(),
             record.file_size);
    if (mysql_query(mysql, sql) != 0)
    {
        return false;
    }

    physical_id = static_cast<long>(mysql_insert_id(mysql));
    return true;
}

bool delete_physical_file_if_unreferenced(MYSQL *mysql, long physical_id)
{
    if (mysql == nullptr || physical_id <= 0)
    {
        return true;
    }
    const std::string sql = "DELETE FROM physical_files WHERE id=" + std::to_string(physical_id) +
                            " AND ref_count=0 AND NOT EXISTS "
                            "(SELECT 1 FROM files WHERE physical_id=" + std::to_string(physical_id) + ")";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool delete_physical_file_if_unreferenced(MYSQL *mysql, long physical_id, bool &deleted)
{
    deleted = false;
    if (mysql == nullptr || physical_id <= 0)
    {
        return true;
    }
    const std::string sql = "DELETE FROM physical_files WHERE id=" + std::to_string(physical_id) +
                            " AND ref_count=0 AND NOT EXISTS "
                            "(SELECT 1 FROM files WHERE physical_id=" + std::to_string(physical_id) + ")";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }
    deleted = mysql_affected_rows(mysql) > 0;
    return true;
}

bool original_name_exists(MYSQL *mysql, const std::string &owner, const std::string &original_name,
                          long folder_id, long ignore_file_id, bool &exists)
{
    if (mysql == nullptr || owner.empty() || original_name.empty())
    {
        return false;
    }

    std::string sql = "SELECT id FROM files WHERE user_id=" + user_id_subquery(mysql, owner) +
                      " AND original_name='" + escape(mysql, original_name) +
                      "' AND " + nullable_id_condition("folder_id", folder_id) +
                      " AND deleted_at IS NULL";
    if (ignore_file_id > 0)
    {
        sql += " AND id<>" + std::to_string(ignore_file_id);
    }
    sql += " LIMIT 1";

    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    exists = mysql_fetch_row(result) != nullptr;
    mysql_free_result(result);
    return true;
}

bool insert_file(MYSQL *mysql, const FileCreateRecord &record, long &file_id)
{
    if (mysql == nullptr || record.owner.empty() || record.stored_name.empty() || record.original_name.empty())
    {
        return false;
    }

    char sql[2048];
    snprintf(sql, sizeof(sql),
             "INSERT INTO files(user_id, stored_name, physical_id, original_name, content_type, folder_id, file_size, is_public, content_sha256) "
             "SELECT id, '%s', %ld, '%s', '%s', %s, %ld, %d, '%s' FROM users WHERE username='%s' AND disabled_at IS NULL",
             escape(mysql, record.stored_name).c_str(),
             record.physical_id,
             escape(mysql, record.original_name).c_str(),
             escape(mysql, record.content_type).c_str(),
             nullable_id_value(record.folder_id).c_str(),
             record.file_size,
             record.is_public ? 1 : 0,
             escape(mysql, record.sha256).c_str(),
             escape(mysql, record.owner).c_str());
    if (mysql_query(mysql, sql) != 0)
    {
        return false;
    }

    if (mysql_affected_rows(mysql) == 0)
    {
        return false;
    }
    file_id = static_cast<long>(mysql_insert_id(mysql));
    return true;
}

bool fetch_user_storage_usage(MYSQL *mysql, const std::string &owner, long &used_bytes)
{
    used_bytes = 0;
    if (mysql == nullptr || owner.empty())
    {
        return false;
    }

    const std::string sql = "SELECT COALESCE(SUM(file_size), 0) FROM files WHERE user_id=" +
                            user_id_subquery(mysql, owner);
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row != nullptr && row[0] != nullptr)
    {
        used_bytes = atol(row[0]);
    }

    mysql_free_result(result);
    return true;
}

bool folder_exists(MYSQL *mysql, const std::string &owner, long folder_id, bool &exists)
{
    exists = folder_id == 0;
    if (folder_id == 0)
    {
        return true;
    }
    if (mysql == nullptr || owner.empty() || folder_id < 0)
    {
        return false;
    }

    const std::string sql = "SELECT id FROM folders WHERE id=" + std::to_string(folder_id) +
                            " AND user_id=" + user_id_subquery(mysql, owner) +
                            " AND deleted_at IS NULL LIMIT 1";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    exists = mysql_fetch_row(result) != nullptr;
    mysql_free_result(result);
    return true;
}

bool folder_name_exists(MYSQL *mysql, const std::string &owner, long parent_id,
                        const std::string &name, bool &exists)
{
    if (mysql == nullptr || owner.empty() || name.empty() || parent_id < 0)
    {
        return false;
    }

    const std::string sql = "SELECT id FROM folders WHERE user_id=" + user_id_subquery(mysql, owner) +
                            " AND " + nullable_id_condition("parent_id", parent_id) +
                            " AND name='" + escape(mysql, name) +
                            "' AND deleted_at IS NULL LIMIT 1";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    exists = mysql_fetch_row(result) != nullptr;
    mysql_free_result(result);
    return true;
}

bool insert_folder(MYSQL *mysql, const FolderCreateRecord &record, long &folder_id)
{
    if (mysql == nullptr || record.owner.empty() || record.name.empty() || record.parent_id < 0)
    {
        return false;
    }

    const std::string sql = "INSERT INTO folders(user_id, parent_id, name) "
                            "SELECT id, " + nullable_id_value(record.parent_id) + ", '" +
                            escape(mysql, record.name) + "' FROM users WHERE username='" +
                            escape(mysql, record.owner) + "' AND disabled_at IS NULL";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }
    if (mysql_affected_rows(mysql) == 0)
    {
        return false;
    }

    folder_id = static_cast<long>(mysql_insert_id(mysql));
    return true;
}

bool fetch_folder(MYSQL *mysql, const std::string &owner, long folder_id, FolderListItem &item)
{
    if (mysql == nullptr || owner.empty() || folder_id <= 0)
    {
        return false;
    }

    const std::string sql = "SELECT id, COALESCE(parent_id, 0), name, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
                            "FROM folders WHERE id=" + std::to_string(folder_id) +
                            " AND user_id=" + user_id_subquery(mysql, owner) +
                            " AND deleted_at IS NULL LIMIT 1";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr)
    {
        mysql_free_result(result);
        return false;
    }

    item.id = row[0] ? atol(row[0]) : 0;
    item.parent_id = row[1] ? atol(row[1]) : 0;
    item.name = row[2] ? row[2] : "";
    item.created_at = row[3] ? row[3] : "";
    mysql_free_result(result);
    return true;
}

bool folder_has_active_children(MYSQL *mysql, const std::string &owner, long folder_id, bool &has_children)
{
    has_children = false;
    if (mysql == nullptr || owner.empty() || folder_id <= 0)
    {
        return false;
    }

    const std::string folder_sql = "SELECT id FROM folders WHERE user_id=" + user_id_subquery(mysql, owner) +
                                   " AND parent_id=" + std::to_string(folder_id) +
                                   " AND deleted_at IS NULL LIMIT 1";
    if (mysql_query(mysql, folder_sql.c_str()) != 0)
    {
        return false;
    }
    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }
    has_children = mysql_fetch_row(result) != nullptr;
    mysql_free_result(result);
    if (has_children)
    {
        return true;
    }

    const std::string file_sql = "SELECT id FROM files WHERE user_id=" + user_id_subquery(mysql, owner) +
                                 " AND folder_id=" + std::to_string(folder_id) +
                                 " AND deleted_at IS NULL LIMIT 1";
    if (mysql_query(mysql, file_sql.c_str()) != 0)
    {
        return false;
    }
    result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }
    has_children = mysql_fetch_row(result) != nullptr;
    mysql_free_result(result);
    return true;
}

bool soft_delete_folder(MYSQL *mysql, const std::string &owner, long folder_id)
{
    if (mysql == nullptr || owner.empty() || folder_id <= 0)
    {
        return false;
    }

    const std::string sql = "UPDATE folders SET deleted_at=NOW(), deleted_marker=id WHERE id=" +
                            std::to_string(folder_id) + " AND user_id=" + user_id_subquery(mysql, owner) +
                            " AND deleted_at IS NULL";
    return mysql_query(mysql, sql.c_str()) == 0 && mysql_affected_rows(mysql) > 0;
}

bool fetch_drive_folders(MYSQL *mysql, const std::string &owner, long parent_id,
                         std::vector<FolderListItem> &items)
{
    items.clear();
    if (mysql == nullptr || owner.empty() || parent_id < 0)
    {
        return false;
    }

    const std::string sql = "SELECT id, COALESCE(parent_id, 0), name, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
                            "FROM folders WHERE user_id=" + user_id_subquery(mysql, owner) +
                            " AND " + nullable_id_condition("parent_id", parent_id) +
                            " AND deleted_at IS NULL ORDER BY name ASC, id ASC";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        FolderListItem item;
        item.id = row[0] ? atol(row[0]) : 0;
        item.parent_id = row[1] ? atol(row[1]) : 0;
        item.name = row[2] ? row[2] : "";
        item.created_at = row[3] ? row[3] : "";
        items.push_back(item);
    }

    mysql_free_result(result);
    return true;
}

bool fetch_drive_files(MYSQL *mysql, const std::string &owner, long folder_id,
                       std::vector<FileListItem> &items)
{
    items.clear();
    if (mysql == nullptr || owner.empty() || folder_id < 0)
    {
        return false;
    }

    const std::string sql = "SELECT id, COALESCE(folder_id, 0), original_name, content_type, file_size, "
                            "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), is_public "
                            "FROM files WHERE user_id=" + user_id_subquery(mysql, owner) +
                            " AND " + nullable_id_condition("folder_id", folder_id) +
                            " AND deleted_at IS NULL ORDER BY original_name ASC, id ASC";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        FileListItem item;
        item.id = row[0] ? atol(row[0]) : 0;
        item.folder_id = row[1] ? atol(row[1]) : 0;
        item.filename = row[2] ? row[2] : "";
        item.content_type = row[3] ? row[3] : "";
        item.size = row[4] ? atol(row[4]) : 0;
        item.created_at = row[5] ? row[5] : "";
        item.is_public = row[6] ? atoi(row[6]) != 0 : false;
        items.push_back(item);
    }

    mysql_free_result(result);
    return true;
}

bool fetch_trash_files(MYSQL *mysql, const std::string &owner, std::vector<FileListItem> &items)
{
    items.clear();
    if (mysql == nullptr || owner.empty())
    {
        return false;
    }

    const std::string sql = "SELECT id, COALESCE(folder_id, 0), original_name, content_type, file_size, "
                            "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), "
                            "DATE_FORMAT(deleted_at, '%Y-%m-%d %H:%i:%s') "
                            "FROM files FORCE INDEX(idx_user_deleted_id) WHERE user_id=" +
                            user_id_subquery(mysql, owner) +
                            " AND deleted_at IS NOT NULL ORDER BY deleted_at DESC, id DESC";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        FileListItem item;
        item.id = row[0] ? atol(row[0]) : 0;
        item.folder_id = row[1] ? atol(row[1]) : 0;
        item.filename = row[2] ? row[2] : "";
        item.content_type = row[3] ? row[3] : "";
        item.size = row[4] ? atol(row[4]) : 0;
        item.created_at = row[5] ? row[5] : "";
        item.deleted_at = row[6] ? row[6] : "";
        item.is_public = false;
        items.push_back(item);
    }

    mysql_free_result(result);
    return true;
}

bool fetch_trash_file_ids(MYSQL *mysql, const std::string &owner, std::vector<long> &ids)
{
    ids.clear();
    if (mysql == nullptr || owner.empty())
    {
        return false;
    }

    const std::string sql = "SELECT id FROM files FORCE INDEX(idx_user_deleted_id) "
                            "WHERE user_id=" + user_id_subquery(mysql, owner) +
                            " AND deleted_at IS NOT NULL ORDER BY deleted_at DESC, id DESC";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        const long id = row[0] ? atol(row[0]) : 0;
        if (id > 0)
        {
            ids.push_back(id);
        }
    }

    mysql_free_result(result);
    return true;
}

bool fetch_public_file_page_ids(MYSQL *mysql, long cursor, long limit, PageIds &page)
{
    if (mysql == nullptr || limit <= 0)
    {
        return false;
    }

    std::string sql = "SELECT id FROM files FORCE INDEX(idx_public_deleted_id) WHERE is_public=1 AND deleted_at IS NULL";
    if (cursor > 0)
    {
        sql += " AND id<" + std::to_string(cursor);
    }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit + 1);
    return fetch_page_ids(mysql, sql, limit, page);
}

bool fetch_public_file_list(MYSQL *mysql, const std::vector<long> &ids, std::vector<FileListItem> &items)
{
    items.clear();
    if (mysql == nullptr)
    {
        return false;
    }
    if (ids.empty())
    {
        return true;
    }

    const std::string sql = "SELECT f.id, f.original_name, f.content_type, f.file_size, "
                            "DATE_FORMAT(f.created_at, '%Y-%m-%d %H:%i:%s'), u.username "
                            "FROM files f JOIN users u ON u.id=f.user_id "
                            "WHERE f.id IN (" + join_numeric_ids(ids) + ") ORDER BY f.id DESC";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        FileListItem item;
        item.id = row[0] ? atol(row[0]) : 0;
        item.filename = row[1] ? row[1] : "";
        item.content_type = row[2] ? row[2] : "";
        item.size = row[3] ? atol(row[3]) : 0;
        item.created_at = row[4] ? row[4] : "";
        item.owner = row[5] ? row[5] : "";
        items.push_back(item);
    }

    mysql_free_result(result);
    return true;
}

bool soft_delete_file(MYSQL *mysql, long file_id)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const std::string sql = "UPDATE files SET deleted_at=NOW(), deleted_marker=id, is_public=0 WHERE id=" +
                            std::to_string(file_id) + " AND deleted_at IS NULL";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool restore_file(MYSQL *mysql, const std::string &owner, long file_id, long folder_id,
                  const std::string &original_name)
{
    if (mysql == nullptr || owner.empty() || file_id <= 0 || folder_id < 0 || original_name.empty())
    {
        return false;
    }

    const std::string sql = "UPDATE files SET deleted_at=NULL, deleted_marker=0, folder_id=" + nullable_id_value(folder_id) +
                            ", original_name='" + escape(mysql, original_name) +
                            "', is_public=0 WHERE id=" + std::to_string(file_id) +
                            " AND user_id=" + user_id_subquery(mysql, owner) +
                            " AND deleted_at IS NOT NULL";
    return mysql_query(mysql, sql.c_str()) == 0 && mysql_affected_rows(mysql) > 0;
}

bool hard_delete_file(MYSQL *mysql, const std::string &owner, long file_id)
{
    if (mysql == nullptr || owner.empty() || file_id <= 0)
    {
        return false;
    }

    const std::string sql = "DELETE FROM files WHERE id=" + std::to_string(file_id) +
                            " AND user_id=" + user_id_subquery(mysql, owner) +
                            " AND deleted_at IS NOT NULL";
    return mysql_query(mysql, sql.c_str()) == 0 && mysql_affected_rows(mysql) > 0;
}

bool update_file_visibility(MYSQL *mysql, long file_id, bool is_public)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const std::string sql = "UPDATE files SET is_public=" + std::string(is_public ? "1" : "0") +
                            " WHERE id=" + std::to_string(file_id);
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool ensure_file_shares_table(MYSQL *mysql)
{
    return mysql != nullptr;
}

bool insert_file_share(MYSQL *mysql, const FileShareCreateRecord &record)
{
    if (mysql == nullptr || record.token.empty() || record.file_id <= 0 || record.owner.empty() ||
        record.expires_in_seconds < 0 || record.max_downloads < 0)
    {
        return false;
    }
    if (!ensure_file_shares_table(mysql))
    {
        return false;
    }

    std::string expires_sql = "NULL";
    if (record.expires_in_seconds > 0)
    {
        expires_sql = "DATE_ADD(NOW(), INTERVAL " + std::to_string(record.expires_in_seconds) + " SECOND)";
    }

    const std::string sql = "INSERT INTO file_shares(token, file_id, user_id, access_code_hash, expires_at, max_downloads) "
                            "SELECT '" + escape(mysql, record.token) + "', " + std::to_string(record.file_id) +
                            ", id, '" + escape(mysql, record.access_code_hash) + "', " +
                            expires_sql + ", " + std::to_string(record.max_downloads) +
                            " FROM users WHERE username='" + escape(mysql, record.owner) +
                            "' AND disabled_at IS NULL";
    return mysql_query(mysql, sql.c_str()) == 0 && mysql_affected_rows(mysql) > 0;
}

bool fetch_file_share(MYSQL *mysql, const std::string &token, FileShareRecord &record)
{
    if (mysql == nullptr || token.empty())
    {
        return false;
    }
    if (!ensure_file_shares_table(mysql))
    {
        return false;
    }

    const std::string sql =
        "SELECT s.token, s.file_id, su.username, s.access_code_hash, "
        "COALESCE(DATE_FORMAT(s.expires_at, '%Y-%m-%d %H:%i:%s'), ''), "
        "s.max_downloads, s.download_count, (s.expires_at IS NOT NULL AND s.expires_at<=NOW()), "
        "COALESCE(f.folder_id, 0), f.file_size, f.is_public, f.deleted_at IS NOT NULL, fu.username, "
        "f.stored_name, f.original_name, f.content_type, f.content_sha256, "
        "COALESCE(DATE_FORMAT(f.deleted_at, '%Y-%m-%d %H:%i:%s'), '') "
        "FROM file_shares s JOIN users su ON su.id=s.user_id "
        "JOIN files f ON f.id=s.file_id JOIN users fu ON fu.id=f.user_id "
        "WHERE s.token='" + escape(mysql, token) + "' LIMIT 1";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (result == nullptr)
    {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr)
    {
        mysql_free_result(result);
        return false;
    }

    record.token = row[0] ? row[0] : "";
    record.file_id = row[1] ? atol(row[1]) : 0;
    record.owner = row[2] ? row[2] : "";
    record.access_code_hash = row[3] ? row[3] : "";
    record.expires_at = row[4] ? row[4] : "";
    record.max_downloads = row[5] ? atol(row[5]) : 0;
    record.download_count = row[6] ? atol(row[6]) : 0;
    record.is_expired = row[7] ? atoi(row[7]) != 0 : false;
    record.file.file_id = record.file_id;
    record.file.folder_id = row[8] ? atol(row[8]) : 0;
    record.file.file_size = row[9] ? atol(row[9]) : 0;
    record.file.is_public = row[10] ? atoi(row[10]) != 0 : false;
    record.file.is_deleted = row[11] ? atoi(row[11]) != 0 : false;
    record.file.owner = row[12] ? row[12] : "";
    record.file.stored_name = row[13] ? row[13] : "";
    record.file.original_name = row[14] ? row[14] : "";
    record.file.content_type = row[15] ? row[15] : "";
    record.file.content_sha256 = row[16] ? row[16] : "";
    record.file.deleted_at = row[17] ? row[17] : "";
    mysql_free_result(result);
    return true;
}

bool increment_file_share_download_count(MYSQL *mysql, const std::string &token)
{
    if (mysql == nullptr || token.empty())
    {
        return false;
    }
    if (!ensure_file_shares_table(mysql))
    {
        return false;
    }

    const std::string sql = "UPDATE file_shares SET download_count=download_count+1 WHERE token='" +
                            escape(mysql, token) +
                            "' AND (expires_at IS NULL OR expires_at>NOW()) "
                            "AND (max_downloads=0 OR download_count<max_downloads)";
    if (mysql_query(mysql, sql.c_str()) != 0)
    {
        return false;
    }
    return mysql_affected_rows(mysql) == 1;
}

}
