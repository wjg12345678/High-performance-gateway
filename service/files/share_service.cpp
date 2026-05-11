#include "share_service.h"

#include "../../http/files/file_store.h"
#include "../../infra/storage/storage.h"
#include "../../repo/mysql/file_repository.h"

#include <openssl/rand.h>

namespace
{
std::string encode_hex(const unsigned char *data, size_t len)
{
    static const char kHexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        hex.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
        hex.push_back(kHexChars[data[i] & 0x0F]);
    }
    return hex;
}

std::string make_share_token()
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    {
        return "";
    }
    return encode_hex(bytes, sizeof(bytes));
}

std::string access_code_hash(const std::string &access_code)
{
    return access_code.empty() ? std::string() : infra_storage::sha256_hex(access_code);
}

void set_not_found(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::NotFound;
    error.message = message;
}

void set_forbidden(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::Forbidden;
    error.message = message;
}

void set_bad_request(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::InvalidArgument;
    error.message = message;
}

void fill_share_result(const repo_mysql::FileShareRecord &record, service_files::ShareResult &result)
{
    result.token = record.token;
    result.file_id = record.file_id;
    result.filename = record.file.original_name;
    result.content_type = record.file.content_type;
    result.size = record.file.file_size;
    result.sha256 = record.file.content_sha256;
    result.has_access_code = !record.access_code_hash.empty();
    result.expires_at = record.expires_at;
    result.max_downloads = record.max_downloads;
    result.download_count = record.download_count;
}

bool load_shareable_file_record(MYSQL *mysql, const std::string &requester, long file_id,
                                ManagedFileRecord &record, service_files::ServiceError &error)
{
    if (!http_file_store::fetch_file_record(mysql, file_id, record))
    {
        set_not_found(error, "file not found");
        return false;
    }
    if (record.owner != requester && !record.is_public)
    {
        set_forbidden(error, "forbidden");
        return false;
    }
    return true;
}

}

namespace service_files
{
std::string build_share_json(const ShareResult &share)
{
    std::string body = "{\"code\":0,\"share\":{\"token\":\"";
    body += json_escape(share.token);
    body += "\",\"file_id\":";
    body += std::to_string(share.file_id);
    body += ",\"filename\":\"";
    body += json_escape(share.filename);
    body += "\",\"content_type\":\"";
    body += json_escape(share.content_type);
    body += "\",\"size\":";
    body += std::to_string(share.size);
    body += ",\"sha256\":\"";
    body += json_escape(share.sha256);
    body += "\",\"has_access_code\":";
    body += share.has_access_code ? "true" : "false";
    body += ",\"expires_at\":";
    if (share.expires_at.empty())
    {
        body += "null";
    }
    else
    {
        body += "\"";
        body += json_escape(share.expires_at);
        body += "\"";
    }
    body += ",\"max_downloads\":";
    body += std::to_string(share.max_downloads);
    body += ",\"download_count\":";
    body += std::to_string(share.download_count);
    body += ",\"share_url\":\"/share?token=";
    body += json_escape(share.token);
    body += "\",\"detail_url\":\"/api/share/";
    body += json_escape(share.token);
    body += "\",\"download_url\":\"/api/share/";
    body += json_escape(share.token);
    body += "/download\"}}";
    return body;
}

bool validate_share_options(const ShareOptions &options, ServiceError &error)
{
    if (options.expires_in_seconds < 0 || options.max_downloads < 0)
    {
        set_bad_request(error, "invalid share limits");
        return false;
    }
    if (options.access_code.size() > 32)
    {
        set_bad_request(error, "access code is too long");
        return false;
    }
    return true;
}

bool validate_share_access_candidate(const repo_mysql::FileShareRecord &record,
                                     const std::string &access_code, ServiceError &error)
{
    if (record.file.is_deleted)
    {
        set_not_found(error, "shared file not found");
        return false;
    }
    if (record.is_expired)
    {
        error.code = service_files::ErrorCode::Gone;
        error.message = "share link expired";
        return false;
    }
    if (record.max_downloads > 0 && record.download_count >= record.max_downloads)
    {
        error.code = service_files::ErrorCode::TooManyRequests;
        error.message = "share download limit reached";
        return false;
    }
    if (!record.access_code_hash.empty())
    {
        if (access_code.empty())
        {
            set_forbidden(error, "access code required");
            return false;
        }
        if (access_code_hash(access_code) != record.access_code_hash)
        {
            set_forbidden(error, "invalid access code");
            return false;
        }
    }
    return true;
}

bool create_share_link(MYSQL *mysql, const std::string &owner, long file_id, const ShareOptions &options,
                       ShareResult &result, ServiceError &error)
{
    if (!validate_share_options(options, error))
    {
        return false;
    }

    ManagedFileRecord file;
    if (!load_shareable_file_record(mysql, owner, file_id, file, error))
    {
        return false;
    }

    repo_mysql::FileShareCreateRecord share;
    share.file_id = file_id;
    share.owner = owner;
    share.access_code_hash = access_code_hash(options.access_code);
    share.expires_in_seconds = options.expires_in_seconds;
    share.max_downloads = options.max_downloads;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        share.token = make_share_token();
        if (share.token.empty())
        {
            return false;
        }
        if (repo_mysql::insert_file_share(mysql, share))
        {
            repo_mysql::FileShareRecord stored;
            if (!repo_mysql::fetch_file_share(mysql, share.token, stored))
            {
                return false;
            }
            fill_share_result(stored, result);
            return true;
        }
    }
    return false;
}

bool load_share_detail(MYSQL *mysql, const std::string &token, const std::string &access_code,
                       ShareResult &result, ServiceError &error)
{
    repo_mysql::FileShareRecord share;
    if (!repo_mysql::fetch_file_share(mysql, token, share))
    {
        set_not_found(error, "share link not found");
        return false;
    }
    if (!validate_share_access_candidate(share, access_code, error))
    {
        return false;
    }

    fill_share_result(share, result);
    return true;
}

bool load_share_download(MYSQL *mysql, const std::string &token, const std::string &access_code,
                         ManagedFileRecord &record, ShareResult &result, ServiceError &error)
{
    repo_mysql::FileShareRecord share;
    if (!repo_mysql::fetch_file_share(mysql, token, share))
    {
        set_not_found(error, "share link not found");
        return false;
    }
    if (!validate_share_access_candidate(share, access_code, error))
    {
        return false;
    }
    if (!repo_mysql::increment_file_share_download_count(mysql, token))
    {
        error.code = ErrorCode::TooManyRequests;
        error.message = "share download limit reached";
        return false;
    }

    share.download_count += 1;
    record = share.file;
    fill_share_result(share, result);
    return true;
}
}
