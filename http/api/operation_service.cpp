#include "../core/connection.h"
#include "../controllers/operation_controller.h"
#include "../../repo/mysql/operation_repository.h"

using namespace std;

bool HttpConnection::write_operation_log(const string &username, const string &action, const string &resource_type,
                                         long resource_id, const string &detail)
{
    return repo_mysql::insert_operation_log(mysql, username, action, resource_type, resource_id, detail);
}

HttpConnection::HTTP_CODE HttpConnection::handle_operation_list()
{
    return http_controllers::OperationController::list(*this);
}

HttpConnection::HTTP_CODE HttpConnection::handle_operation_delete(const char *path)
{
    return http_controllers::OperationController::remove(*this, path);
}
