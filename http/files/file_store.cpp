#include "file_store.h"

#include "../../repo/mysql/file_repository.h"

namespace http_file_store
{
bool fetch_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, bool include_deleted)
{
    return repo_mysql::fetch_file_record(mysql, file_id, record, include_deleted);
}
}
