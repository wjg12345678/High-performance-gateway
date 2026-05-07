#include "file_store.h"

#include <cstdlib>
#include <string>

namespace http_file_store
{
bool fetch_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, bool include_deleted)
{
    if (mysql == nullptr)
    {
        return false;
    }

    char query[768];
    snprintf(query, sizeof(query),
             "SELECT owner_username, stored_name, original_name, content_type, file_size, is_public, "
             "COALESCE(content_sha256, ''), COALESCE(DATE_FORMAT(deleted_at, '%%Y-%%m-%%d %%H:%%i:%%s'), '') "
             "FROM files WHERE id=%ld%s LIMIT 1",
             file_id,
             include_deleted ? "" : " AND deleted_at IS NULL");
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
    record.is_deleted = !record.deleted_at.empty();
    mysql_free_result(result);
    return true;
}
}
