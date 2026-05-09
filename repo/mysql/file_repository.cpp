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
}

bool fetch_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, bool include_deleted)
{
    if (mysql == nullptr)
    {
        return false;
    }

    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT f.owner_username, COALESCE(NULLIF(p.stored_name, ''), f.stored_name), f.original_name, f.content_type, f.file_size, f.is_public, "
             "COALESCE(f.content_sha256, ''), COALESCE(DATE_FORMAT(f.deleted_at, '%%Y-%%m-%%d %%H:%%i:%%s'), ''), "
             "COALESCE(f.folder_id, 0), COALESCE(f.physical_id, 0) "
             "FROM files f LEFT JOIN physical_files p ON f.physical_id=p.id WHERE f.id=%ld%s LIMIT 1",
             file_id,
             include_deleted ? "" : " AND f.deleted_at IS NULL");
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
    if (row == nullptr)
    {
        mysql_free_result(result);
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
    mysql_free_result(result);
    return true;
}

unsigned int last_errno(MYSQL *mysql)
{
    return mysql == nullptr ? 0 : mysql_errno(mysql);
}

bool fetch_physical_file_by_sha256(MYSQL *mysql, const std::string &sha256, PhysicalFileRecord &record)
{
    if (mysql == nullptr || sha256.empty())
    {
        return false;
    }

    const std::string sql = "SELECT id, sha256, stored_name, file_size, ref_count FROM physical_files WHERE sha256='" +
                            escape(mysql, sha256) + "' LIMIT 1";
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

    record.id = row[0] ? atol(row[0]) : 0;
    record.sha256 = row[1] ? row[1] : "";
    record.stored_name = row[2] ? row[2] : "";
    record.file_size = row[3] ? atol(row[3]) : 0;
    record.ref_count = row[4] ? atol(row[4]) : 0;
    mysql_free_result(result);
    return record.id > 0 && !record.stored_name.empty();
}

bool insert_physical_file(MYSQL *mysql, const PhysicalFileRecord &record, long &physical_id)
{
    if (mysql == nullptr || record.sha256.empty() || record.stored_name.empty())
    {
        return false;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO physical_files(sha256, stored_name, file_size, ref_count) VALUES('%s', '%s', %ld, %ld)",
             escape(mysql, record.sha256).c_str(),
             escape(mysql, record.stored_name).c_str(),
             record.file_size,
             record.ref_count);
    if (mysql_query(mysql, sql) != 0)
    {
        return false;
    }

    physical_id = static_cast<long>(mysql_insert_id(mysql));
    return true;
}

bool increment_physical_ref(MYSQL *mysql, long physical_id)
{
    if (mysql == nullptr || physical_id <= 0)
    {
        return false;
    }
    const std::string sql = "UPDATE physical_files SET ref_count=ref_count+1 WHERE id=" + std::to_string(physical_id);
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool decrement_physical_ref(MYSQL *mysql, long physical_id)
{
    if (mysql == nullptr || physical_id <= 0)
    {
        return true;
    }
    const std::string sql = "UPDATE physical_files SET ref_count=GREATEST(ref_count-1, 0) WHERE id=" + std::to_string(physical_id);
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool delete_physical_file_if_unreferenced(MYSQL *mysql, long physical_id)
{
    if (mysql == nullptr || physical_id <= 0)
    {
        return true;
    }
    const std::string sql = "DELETE FROM physical_files WHERE id=" + std::to_string(physical_id) + " AND ref_count=0";
    return mysql_query(mysql, sql.c_str()) == 0;
}

bool original_name_exists(MYSQL *mysql, const std::string &owner, const std::string &original_name,
                          long folder_id, long ignore_file_id, bool &exists)
{
    if (mysql == nullptr || owner.empty() || original_name.empty())
    {
        return false;
    }

    std::string sql = "SELECT id FROM files WHERE owner_username='" + escape(mysql, owner) +
                      "' AND original_name='" + escape(mysql, original_name) +
                      "' AND folder_id=" + std::to_string(folder_id) +
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
             "INSERT INTO files(owner_username, stored_name, physical_id, original_name, content_type, folder_id, file_size, is_public, content_sha256) "
             "VALUES('%s', '%s', %ld, '%s', '%s', %ld, %ld, %d, '%s')",
             escape(mysql, record.owner).c_str(),
             escape(mysql, record.stored_name).c_str(),
             record.physical_id,
             escape(mysql, record.original_name).c_str(),
             escape(mysql, record.content_type).c_str(),
             record.folder_id,
             record.file_size,
             record.is_public ? 1 : 0,
             escape(mysql, record.sha256).c_str());
    if (mysql_query(mysql, sql) != 0)
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

    const std::string sql = "SELECT COALESCE(SUM(file_size), 0) FROM files WHERE owner_username='" +
                            escape(mysql, owner) + "'";
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
                            " AND owner_username='" + escape(mysql, owner) +
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

bool folder_name_exists(MYSQL *mysql, const std::string &owner, long parent_id,
                        const std::string &name, bool &exists)
{
    if (mysql == nullptr || owner.empty() || name.empty() || parent_id < 0)
    {
        return false;
    }

    const std::string sql = "SELECT id FROM folders WHERE owner_username='" + escape(mysql, owner) +
                            "' AND parent_id=" + std::to_string(parent_id) +
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

    const std::string sql = "INSERT INTO folders(owner_username, parent_id, name) VALUES('" +
                            escape(mysql, record.owner) + "', " + std::to_string(record.parent_id) +
                            ", '" + escape(mysql, record.name) + "')";
    if (mysql_query(mysql, sql.c_str()) != 0)
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

    const std::string sql = "SELECT id, parent_id, name, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
                            "FROM folders WHERE id=" + std::to_string(folder_id) +
                            " AND owner_username='" + escape(mysql, owner) +
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

    const std::string folder_sql = "SELECT id FROM folders WHERE owner_username='" + escape(mysql, owner) +
                                   "' AND parent_id=" + std::to_string(folder_id) +
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

    const std::string file_sql = "SELECT id FROM files WHERE owner_username='" + escape(mysql, owner) +
                                 "' AND folder_id=" + std::to_string(folder_id) +
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
                            std::to_string(folder_id) + " AND owner_username='" + escape(mysql, owner) +
                            "' AND deleted_at IS NULL";
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

    const std::string sql = "SELECT id, parent_id, name, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s') "
                            "FROM folders WHERE owner_username='" + escape(mysql, owner) +
                            "' AND parent_id=" + std::to_string(parent_id) +
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

    const std::string sql = "SELECT id, folder_id, original_name, content_type, file_size, "
                            "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), is_public "
                            "FROM files WHERE owner_username='" + escape(mysql, owner) +
                            "' AND folder_id=" + std::to_string(folder_id) +
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

bool fetch_private_file_page_ids(MYSQL *mysql, const std::string &owner, bool include_deleted,
                                 long cursor, long limit, PageIds &page)
{
    if (mysql == nullptr || owner.empty() || limit <= 0)
    {
        return false;
    }

    std::string sql = "SELECT id FROM files FORCE INDEX(idx_owner_deleted_id) WHERE owner_username='" +
                      escape(mysql, owner) + "'";
    sql += include_deleted ? " AND deleted_at IS NOT NULL" : " AND deleted_at IS NULL";
    if (cursor > 0)
    {
        sql += " AND id<" + std::to_string(cursor);
    }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit + 1);
    return fetch_page_ids(mysql, sql, limit, page);
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

bool fetch_private_file_list(MYSQL *mysql, const std::vector<long> &ids, std::vector<FileListItem> &items)
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

    const std::string sql = "SELECT id, original_name, content_type, file_size, "
                            "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), is_public, folder_id, "
                            "COALESCE(DATE_FORMAT(deleted_at, '%Y-%m-%d %H:%i:%s'), '') "
                            "FROM files WHERE id IN (" + join_numeric_ids(ids) + ") ORDER BY id DESC";
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
        item.is_public = row[5] ? atoi(row[5]) != 0 : false;
        item.folder_id = row[6] ? atol(row[6]) : 0;
        item.deleted_at = row[7] ? row[7] : "";
        items.push_back(item);
    }

    mysql_free_result(result);
    return true;
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

    const std::string sql = "SELECT id, original_name, content_type, file_size, "
                            "DATE_FORMAT(created_at, '%Y-%m-%d %H:%i:%s'), owner_username "
                            "FROM files WHERE id IN (" + join_numeric_ids(ids) + ") ORDER BY id DESC";
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

    const std::string sql = "UPDATE files SET deleted_at=NOW(), is_public=0 WHERE id=" +
                            std::to_string(file_id) + " AND deleted_at IS NULL";
    return mysql_query(mysql, sql.c_str()) == 0;
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

bool restore_file(MYSQL *mysql, long file_id, const std::string &restored_name)
{
    if (mysql == nullptr || restored_name.empty())
    {
        return false;
    }

    const std::string sql = "UPDATE files SET deleted_at=NULL, is_public=0, original_name='" +
                            escape(mysql, restored_name) + "' WHERE id=" + std::to_string(file_id);
    return mysql_query(mysql, sql.c_str()) == 0;
}
bool ensure_file_shares_table(MYSQL *mysql)
{
    if (mysql == nullptr)
    {
        return false;
    }

    const std::string sql =
        "CREATE TABLE IF NOT EXISTS file_shares ("
        "token VARCHAR(64) NOT NULL,"
        "file_id BIGINT NOT NULL,"
        "owner_username VARCHAR(50) NOT NULL,"
        "access_code_hash CHAR(64) NOT NULL DEFAULT '',"
        "expires_at TIMESTAMP NULL DEFAULT NULL,"
        "max_downloads BIGINT NOT NULL DEFAULT 0,"
        "download_count BIGINT NOT NULL DEFAULT 0,"
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "PRIMARY KEY (token),"
        "KEY idx_file_id (file_id),"
        "KEY idx_owner_created (owner_username, created_at)"
        ") ENGINE=InnoDB";
    return mysql_query(mysql, sql.c_str()) == 0;
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

    const std::string sql = "INSERT INTO file_shares(token, file_id, owner_username, access_code_hash, expires_at, max_downloads) VALUES('" +
                            escape(mysql, record.token) + "', " + std::to_string(record.file_id) + ", '" +
                            escape(mysql, record.owner) + "', '" + escape(mysql, record.access_code_hash) + "', " +
                            expires_sql + ", " + std::to_string(record.max_downloads) + ")";
    return mysql_query(mysql, sql.c_str()) == 0;
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
        "SELECT s.token, s.file_id, s.owner_username, s.access_code_hash, "
        "COALESCE(DATE_FORMAT(s.expires_at, '%Y-%m-%d %H:%i:%s'), ''), "
        "s.max_downloads, s.download_count, (s.expires_at IS NOT NULL AND s.expires_at<=NOW()), "
        "f.folder_id, f.file_size, f.is_public, f.deleted_at IS NOT NULL, f.owner_username, "
        "f.stored_name, f.original_name, f.content_type, f.content_sha256, "
        "COALESCE(DATE_FORMAT(f.deleted_at, '%Y-%m-%d %H:%i:%s'), '') "
        "FROM file_shares s JOIN files f ON f.id=s.file_id "
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
