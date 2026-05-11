#ifndef HTTP_CONTROLLERS_FILE_CONTROLLER_H
#define HTTP_CONTROLLERS_FILE_CONTROLLER_H

#include "../core/http_message.h"

namespace http_controllers
{
class FileController
{
public:
    static http_core::HttpCode upload_preflight(http_core::HttpRequest &request,
                                                http_core::RequestContext &context,
                                                http_core::HttpResponse &response);
    static http_core::HttpCode upload(http_core::HttpRequest &request,
                                      http_core::RequestContext &context,
                                      http_core::HttpResponse &response);
    static http_core::HttpCode drive_item_list(http_core::HttpRequest &request,
                                               http_core::RequestContext &context,
                                               http_core::HttpResponse &response);
    static http_core::HttpCode trash_item_list(http_core::HttpRequest &request,
                                               http_core::RequestContext &context,
                                               http_core::HttpResponse &response);
    static http_core::HttpCode empty_trash(http_core::HttpRequest &request,
                                           http_core::RequestContext &context,
                                           http_core::HttpResponse &response);
    static http_core::HttpCode drive_folder_create(http_core::HttpRequest &request,
                                                   http_core::RequestContext &context,
                                                   http_core::HttpResponse &response);
    static http_core::HttpCode drive_folder_delete(http_core::HttpRequest &request,
                                                   http_core::RequestContext &context,
                                                   http_core::HttpResponse &response,
                                                   const char *path);
    static http_core::HttpCode drive_file_upload(http_core::HttpRequest &request,
                                                 http_core::RequestContext &context,
                                                 http_core::HttpResponse &response);
    static http_core::HttpCode public_file_list(http_core::HttpRequest &request,
                                                http_core::RequestContext &context,
                                                http_core::HttpResponse &response);
    static http_core::HttpCode public_file_detail(http_core::HttpRequest &request,
                                                  http_core::RequestContext &context,
                                                  http_core::HttpResponse &response,
                                                  const char *path);
    static http_core::HttpCode private_file_download(http_core::HttpRequest &request,
                                                     http_core::RequestContext &context,
                                                     http_core::HttpResponse &response,
                                                     const char *path);
    static http_core::HttpCode public_file_download(http_core::HttpRequest &request,
                                                    http_core::RequestContext &context,
                                                    http_core::HttpResponse &response,
                                                    const char *path);
    static http_core::HttpCode share_create(http_core::HttpRequest &request,
                                            http_core::RequestContext &context,
                                            http_core::HttpResponse &response,
                                            const char *path);
    static http_core::HttpCode share_detail(http_core::HttpRequest &request,
                                            http_core::RequestContext &context,
                                            http_core::HttpResponse &response,
                                            const char *path);
    static http_core::HttpCode share_download(http_core::HttpRequest &request,
                                              http_core::RequestContext &context,
                                              http_core::HttpResponse &response,
                                              const char *path);
    static http_core::HttpCode remove(http_core::HttpRequest &request,
                                      http_core::RequestContext &context,
                                      http_core::HttpResponse &response,
                                      const char *path);
    static http_core::HttpCode restore(http_core::HttpRequest &request,
                                       http_core::RequestContext &context,
                                       http_core::HttpResponse &response,
                                       const char *path);
    static http_core::HttpCode remove_permanently(http_core::HttpRequest &request,
                                                  http_core::RequestContext &context,
                                                  http_core::HttpResponse &response,
                                                  const char *path);
    static http_core::HttpCode update_visibility(http_core::HttpRequest &request,
                                                 http_core::RequestContext &context,
                                                 http_core::HttpResponse &response,
                                                 const char *path);
};
}

#endif
