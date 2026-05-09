#ifndef HTTP_CONTROLLERS_FILE_CONTROLLER_H
#define HTTP_CONTROLLERS_FILE_CONTROLLER_H

#include "../core/connection.h"

namespace http_controllers
{
class FileController
{
public:
    static HttpConnection::HTTP_CODE upload_preflight(HttpConnection &conn);
    static HttpConnection::HTTP_CODE upload(HttpConnection &conn);
    static HttpConnection::HTTP_CODE drive_item_list(HttpConnection &conn);
    static HttpConnection::HTTP_CODE drive_folder_create(HttpConnection &conn);
    static HttpConnection::HTTP_CODE drive_folder_delete(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE drive_file_upload(HttpConnection &conn);
    static HttpConnection::HTTP_CODE private_file_list(HttpConnection &conn);
    static HttpConnection::HTTP_CODE public_file_list(HttpConnection &conn);
    static HttpConnection::HTTP_CODE public_file_detail(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE private_file_download(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE public_file_download(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE share_create(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE share_detail(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE share_download(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE remove(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE update_visibility(HttpConnection &conn, const char *path);
    static HttpConnection::HTTP_CODE restore(HttpConnection &conn, const char *path);
};
}

#endif
