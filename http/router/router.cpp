#include "../core/connection.h"

#include <cstring>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

using namespace std;

namespace
{
bool starts_with_ignore_case(const char *text, const char *prefix)
{
    return strncasecmp(text, prefix, strlen(prefix)) == 0;
}

struct RouteEntry
{
    HttpConnection::METHOD method;
    const char *path;
    HttpConnection::RouteHandler handler;
};

struct StaticAlias
{
    const char *route_path;
    const char *file_path;
    bool legacy_only;
};

bool route_matches(const RouteEntry &route, HttpConnection::METHOD method, const char *url)
{
    return route.method == method && strcasecmp(url, route.path) == 0;
}

bool route_path_matches(const RouteEntry &route, const char *url)
{
    return strcasecmp(url, route.path) == 0;
}

const char *detect_static_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == nullptr)
    {
        return "application/octet-stream";
    }

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)
    {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(dot, ".css") == 0)
    {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(dot, ".js") == 0)
    {
        return "application/javascript; charset=utf-8";
    }
    if (strcasecmp(dot, ".json") == 0)
    {
        return "application/json; charset=utf-8";
    }
    if (strcasecmp(dot, ".svg") == 0)
    {
        return "image/svg+xml";
    }
    if (strcasecmp(dot, ".png") == 0)
    {
        return "image/png";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0)
    {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".gif") == 0)
    {
        return "image/gif";
    }
    if (strcasecmp(dot, ".webp") == 0)
    {
        return "image/webp";
    }
    if (strcasecmp(dot, ".mp4") == 0)
    {
        return "video/mp4";
    }
    if (strcasecmp(dot, ".txt") == 0)
    {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}
}

HttpConnection::HTTP_CODE HttpConnection::route_request()
{
    static const RouteEntry kRoutes[] = {
        {POST, "/login", &HttpConnection::route_page_login},
        {POST, "/register", &HttpConnection::route_page_register},
        {POST, "/api/login", &HttpConnection::route_api_login},
        {POST, "/api/register", &HttpConnection::route_api_register},
        {POST, "/api/echo", &HttpConnection::route_api_echo},
        {GET, "/api/private/ping", &HttpConnection::route_api_private_ping},
        {POST, "/api/private/logout", &HttpConnection::route_api_private_logout},
        {GET, "/healthz", &HttpConnection::route_healthz},
        {HEAD, "/healthz", &HttpConnection::route_healthz},
    };
    static const RouteEntry kLegacyRoutes[] = {
        {POST, "/2", &HttpConnection::route_page_login},
        {POST, "/2CGISQL.cgi", &HttpConnection::route_page_login},
        {POST, "/3", &HttpConnection::route_page_register},
        {POST, "/3CGISQL.cgi", &HttpConnection::route_page_register},
        {GET, "/0", &HttpConnection::route_register_page},
        {GET, "/1", &HttpConnection::route_login_page},
        {GET, "/5", &HttpConnection::route_picture_page},
        {GET, "/6", &HttpConnection::route_video_page},
        {GET, "/7", &HttpConnection::route_fans_page},
        {HEAD, "/0", &HttpConnection::route_register_page},
        {HEAD, "/1", &HttpConnection::route_login_page},
        {HEAD, "/5", &HttpConnection::route_picture_page},
        {HEAD, "/6", &HttpConnection::route_video_page},
        {HEAD, "/7", &HttpConnection::route_fans_page},
    };

    for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); ++i)
    {
        if (route_matches(kRoutes[i], m_method, m_url))
        {
            return (this->*kRoutes[i].handler)();
        }
    }

    if (m_legacy_compat_enabled)
    {
        for (size_t i = 0; i < sizeof(kLegacyRoutes) / sizeof(kLegacyRoutes[0]); ++i)
        {
            if (route_matches(kLegacyRoutes[i], m_method, m_url))
            {
                return (this->*kLegacyRoutes[i].handler)();
            }
        }
    }

    if (starts_with_ignore_case(m_url, "/api/"))
    {
        return handle_api_request();
    }

    return handle_static_route();
}

HttpConnection::HTTP_CODE HttpConnection::route_api_echo()
{
    string response = "{\"code\":0,\"content_type\":\"" + json_escape(m_content_type) +
                      "\",\"body\":\"" + json_escape(m_request_body) + "\"}";
    set_memory_response(200, "OK", response, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::route_api_private_ping()
{
    string body = string("{\"code\":0,\"message\":\"pong\",\"user\":\"") +
                  json_escape(m_current_user) + "\"}";
    set_memory_response(200, "OK", body, "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::route_healthz()
{
    set_memory_response(200, "OK", "{\"code\":0,\"status\":\"ok\"}", "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::route_page_login()
{
    return handle_auth_request(false, false);
}

HttpConnection::HTTP_CODE HttpConnection::route_page_register()
{
    return handle_auth_request(true, false);
}

HttpConnection::HTTP_CODE HttpConnection::route_register_page()
{
    return resolve_static_path("/register.html") ? open_static_file() : BAD_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::route_login_page()
{
    return resolve_static_path("/login.html") ? open_static_file() : BAD_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::route_picture_page()
{
    return resolve_static_path("/files.html") ? open_static_file() : BAD_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::route_video_page()
{
    return resolve_static_path("/files.html") ? open_static_file() : BAD_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::route_fans_page()
{
    return resolve_static_path("/files.html") ? open_static_file() : BAD_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_static_route()
{
    if (!resolve_static_path(m_url))
    {
        return BAD_REQUEST;
    }
    return open_static_file();
}

HttpConnection::HTTP_CODE HttpConnection::open_static_file()
{
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    m_filefd = open(m_real_file, O_RDONLY);
    if (m_filefd < 0)
        return NO_RESOURCE;
    strncpy(m_response_content_type, detect_static_content_type(m_real_file), sizeof(m_response_content_type) - 1);
    return FILE_REQUEST;
}

bool HttpConnection::resolve_static_path(const char *route_path)
{
    const char *target = route_path;
    static const StaticAlias kStaticAliases[] = {
        {"/", "/index.html", false},
        {"/index", "/index.html", false},
        {"/login", "/login.html", false},
        {"/register", "/register.html", false},
        {"/welcome", "/welcome.html", false},
        {"/files", "/files.html", false},
        {"/share", "/share.html", false},
        {"/media", "/media.html", false},
        {"/media/photo", "/media-photo.html", false},
        {"/media/video", "/media-video.html", false},
        {"/login-error", "/login-error.html", false},
        {"/register-error", "/register-error.html", false},
        {"/0", "/register.html", true},
        {"/1", "/login.html", true},
        {"/5", "/files.html", true},
        {"/6", "/files.html", true},
        {"/7", "/files.html", true},
    };

    for (size_t i = 0; i < sizeof(kStaticAliases) / sizeof(kStaticAliases[0]); ++i)
    {
        if ((!kStaticAliases[i].legacy_only || m_legacy_compat_enabled) &&
            strcmp(route_path, kStaticAliases[i].route_path) == 0)
        {
            target = kStaticAliases[i].file_path;
            break;
        }
    }

    if (target == nullptr || target[0] != '/')
    {
        return false;
    }

    const int written = snprintf(m_real_file, sizeof(m_real_file), "%s%s", doc_root.c_str(), target);
    return written > 0 && written < static_cast<int>(sizeof(m_real_file));
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_request()
{
    static const RouteEntry kExactApiRoutes[] = {
        {GET, "/healthz", &HttpConnection::route_healthz},
        {HEAD, "/healthz", &HttpConnection::route_healthz},
        {POST, "/api/login", &HttpConnection::route_api_login},
        {POST, "/api/register", &HttpConnection::route_api_register},
        {POST, "/api/echo", &HttpConnection::route_api_echo},
        {GET, "/api/private/ping", &HttpConnection::route_api_private_ping},
        {POST, "/api/private/logout", &HttpConnection::route_api_private_logout},
        {GET, "/api/files/public", &HttpConnection::handle_public_file_list},
        {GET, "/api/private/operations", &HttpConnection::handle_operation_list},
    };

    for (size_t i = 0; i < sizeof(kExactApiRoutes) / sizeof(kExactApiRoutes[0]); ++i)
    {
        if (route_matches(kExactApiRoutes[i], m_method, m_url))
        {
            return (this->*kExactApiRoutes[i].handler)();
        }
    }

    for (size_t i = 0; i < sizeof(kExactApiRoutes) / sizeof(kExactApiRoutes[0]); ++i)
    {
        if (route_path_matches(kExactApiRoutes[i], m_url))
        {
            return NOT_IMPLEMENTED;
        }
    }

    if (strcasecmp(m_url, "/api/private/files") == 0 ||
        strcasecmp(m_url, "/api/private/files/preflight") == 0)
    {
        return handle_api_private_files_collection();
    }
    if (starts_with_ignore_case(m_url, "/api/private/files/"))
    {
        return handle_api_private_file_item(m_url + strlen("/api/private/files/"));
    }
    if (starts_with_ignore_case(m_url, "/api/private/operations/"))
    {
        return m_method == DELETE ? handle_operation_delete(m_url + strlen("/api/private/operations/")) : NOT_IMPLEMENTED;
    }
    if (starts_with_ignore_case(m_url, "/api/drive/"))
    {
        return handle_api_drive_request();
    }
    if (starts_with_ignore_case(m_url, "/api/files/public/"))
    {
        return handle_api_public_file_item(m_url + strlen("/api/files/public/"));
    }
    if (starts_with_ignore_case(m_url, "/api/share/"))
    {
        return handle_api_share_item(m_url + strlen("/api/share/"));
    }

    set_memory_response(404, "Not Found", "{\"code\":404,\"message\":\"api not found\"}", "application/json");
    return MEMORY_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_private_files_collection()
{
    if (strcasecmp(m_url, "/api/private/files/preflight") == 0)
    {
        return m_method == POST ? handle_file_upload_preflight() : NOT_IMPLEMENTED;
    }
    if (m_method == POST)
    {
        return handle_file_upload();
    }
    if (m_method == GET)
    {
        return handle_file_list();
    }
    return NOT_IMPLEMENTED;
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_private_file_item(const char *path)
{
    if (m_method == GET && strstr(path, "/download") != nullptr)
    {
        return handle_file_download(path);
    }
    if (m_method == POST && strstr(path, "/share") != nullptr)
    {
        return handle_file_share_create(path);
    }
    if (m_method == POST && strstr(path, "/visibility") != nullptr)
    {
        return handle_file_visibility_update(path);
    }
    if (m_method == POST && strstr(path, "/restore") != nullptr)
    {
        return handle_file_restore(path);
    }
    if (m_method == DELETE)
    {
        return handle_file_delete(path);
    }
    return NOT_IMPLEMENTED;
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_drive_request()
{
    if (strcasecmp(m_url, "/api/drive/items") == 0)
    {
        return m_method == GET ? handle_drive_item_list() : NOT_IMPLEMENTED;
    }
    if (strcasecmp(m_url, "/api/drive/folders") == 0)
    {
        return m_method == POST ? handle_drive_folder_create() : NOT_IMPLEMENTED;
    }
    if (strcasecmp(m_url, "/api/drive/files/upload") == 0)
    {
        return m_method == POST ? handle_drive_file_upload() : NOT_IMPLEMENTED;
    }
    if (strcasecmp(m_url, "/api/drive/files/preflight") == 0)
    {
        return m_method == POST ? handle_file_upload_preflight() : NOT_IMPLEMENTED;
    }
    if (starts_with_ignore_case(m_url, "/api/drive/folders/"))
    {
        return handle_api_drive_folder_item(m_url + strlen("/api/drive/folders/"));
    }
    if (starts_with_ignore_case(m_url, "/api/drive/files/"))
    {
        return handle_api_drive_file_item(m_url + strlen("/api/drive/files/"));
    }
    return NOT_IMPLEMENTED;
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_drive_folder_item(const char *path)
{
    return m_method == DELETE ? handle_drive_folder_delete(path) : NOT_IMPLEMENTED;
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_drive_file_item(const char *path)
{
    if (m_method == GET && strstr(path, "/download") != nullptr)
    {
        return handle_file_download(path);
    }
    if (m_method == DELETE)
    {
        return handle_file_delete(path);
    }
    return NOT_IMPLEMENTED;
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_public_file_item(const char *path)
{
    if (m_method == GET && strstr(path, "/download") != nullptr)
    {
        return handle_public_file_download(path);
    }
    if (m_method == GET)
    {
        return handle_public_file_detail(path);
    }
    return NOT_IMPLEMENTED;
}

HttpConnection::HTTP_CODE HttpConnection::handle_api_share_item(const char *path)
{
    if (m_method == GET && strstr(path, "/download") != nullptr)
    {
        return handle_share_download(path);
    }
    if (m_method == GET)
    {
        return handle_share_detail(path);
    }
    return NOT_IMPLEMENTED;
}
