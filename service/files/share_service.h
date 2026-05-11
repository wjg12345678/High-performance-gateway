#ifndef SERVICE_FILES_SHARE_SERVICE_H
#define SERVICE_FILES_SHARE_SERVICE_H

#include "file_service.h"

#include <string>

namespace service_files
{
struct ShareOptions
{
    std::string access_code;
    long expires_in_seconds;
    long max_downloads;

    ShareOptions() : expires_in_seconds(0), max_downloads(0) {}
};

struct ShareResult
{
    std::string token;
    long file_id;
    std::string filename;
    std::string content_type;
    long size;
    std::string sha256;
    bool has_access_code;
    std::string expires_at;
    long max_downloads;
    long download_count;

    ShareResult() : file_id(0), size(0), has_access_code(false), max_downloads(0), download_count(0) {}
};

std::string build_share_json(const ShareResult &share);

bool validate_share_options(const ShareOptions &options, ServiceError &error);
bool validate_share_access_candidate(const repo_mysql::FileShareRecord &record,
                                     const std::string &access_code, ServiceError &error);
bool create_share_link(MYSQL *mysql, const std::string &owner, long file_id, const ShareOptions &options,
                       ShareResult &result, ServiceError &error);
bool load_share_detail(MYSQL *mysql, const std::string &token, const std::string &access_code,
                       ShareResult &result, ServiceError &error);
bool load_share_download(MYSQL *mysql, const std::string &token, const std::string &access_code,
                         ManagedFileRecord &record, ShareResult &result, ServiceError &error);
}

#endif
