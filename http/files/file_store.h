#ifndef HTTP_FILE_STORE_H
#define HTTP_FILE_STORE_H

#include <mysql/mysql.h>

#include "file_types.h"

namespace http_file_store
{
bool fetch_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record);
}

#endif
