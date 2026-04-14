#include "file_store.h"

#include <cstdlib>
#include <string>

namespace http_file_store
{
bool fetch_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record)
{
    if (mysql == nullptr)
    {
        return false;
    }

    char query[512];
    snprintf(query, sizeof(query),
             "SELECT owner_username, stored_name, original_name, content_type, file_size, is_public "
             "FROM files WHERE id=%ld LIMIT 1",
             file_id);
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
    mysql_free_result(result);
    return true;
}
}
