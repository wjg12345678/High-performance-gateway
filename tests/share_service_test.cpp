#include <cassert>
#include <string>

#include "../infra/storage/storage.h"
#include "../service/files/share_service.h"

static repo_mysql::FileShareRecord share_record()
{
    repo_mysql::FileShareRecord record;
    record.token = "abc";
    record.file_id = 7;
    record.owner = "alice";
    record.max_downloads = 0;
    record.download_count = 0;
    record.is_expired = false;
    record.file.file_id = 7;
    record.file.owner = "alice";
    record.file.original_name = "report.txt";
    record.file.content_type = "text/plain";
    record.file.file_size = 42;
    record.file.content_sha256 = "hash";
    record.file.is_deleted = false;
    return record;
}

static void accepts_valid_share_options()
{
    service_files::ShareOptions options;
    options.access_code = "1234";
    options.expires_in_seconds = 3600;
    options.max_downloads = 5;
    service_files::ServiceError error;

    assert(service_files::validate_share_options(options, error));
}

static void rejects_negative_share_limits()
{
    service_files::ShareOptions options;
    options.expires_in_seconds = -1;
    service_files::ServiceError error;

    assert(!service_files::validate_share_options(options, error));
    assert(error.code == service_files::ErrorCode::InvalidArgument);
    assert(error.message == "invalid share limits");
}

static void rejects_long_access_code()
{
    service_files::ShareOptions options;
    options.access_code = std::string(33, 'x');
    service_files::ServiceError error;

    assert(!service_files::validate_share_options(options, error));
    assert(error.code == service_files::ErrorCode::InvalidArgument);
    assert(error.message == "access code is too long");
}

static void accepts_public_share_without_access_code()
{
    repo_mysql::FileShareRecord record = share_record();
    service_files::ServiceError error;

    assert(service_files::validate_share_access_candidate(record, "", error));
}

static void rejects_deleted_shared_file()
{
    repo_mysql::FileShareRecord record = share_record();
    record.file.is_deleted = true;
    service_files::ServiceError error;

    assert(!service_files::validate_share_access_candidate(record, "", error));
    assert(error.code == service_files::ErrorCode::NotFound);
    assert(error.message == "shared file not found");
}

static void rejects_expired_share()
{
    repo_mysql::FileShareRecord record = share_record();
    record.is_expired = true;
    service_files::ServiceError error;

    assert(!service_files::validate_share_access_candidate(record, "", error));
    assert(error.code == service_files::ErrorCode::Gone);
    assert(error.message == "share link expired");
}

static void rejects_download_limit()
{
    repo_mysql::FileShareRecord record = share_record();
    record.max_downloads = 3;
    record.download_count = 3;
    service_files::ServiceError error;

    assert(!service_files::validate_share_access_candidate(record, "", error));
    assert(error.code == service_files::ErrorCode::TooManyRequests);
    assert(error.message == "share download limit reached");
}

static void rejects_missing_access_code()
{
    repo_mysql::FileShareRecord record = share_record();
    record.access_code_hash = infra_storage::sha256_hex("1234");
    service_files::ServiceError error;

    assert(!service_files::validate_share_access_candidate(record, "", error));
    assert(error.code == service_files::ErrorCode::Forbidden);
    assert(error.message == "access code required");
}

static void rejects_invalid_access_code()
{
    repo_mysql::FileShareRecord record = share_record();
    record.access_code_hash = infra_storage::sha256_hex("1234");
    service_files::ServiceError error;

    assert(!service_files::validate_share_access_candidate(record, "0000", error));
    assert(error.code == service_files::ErrorCode::Forbidden);
    assert(error.message == "invalid access code");
}

static void accepts_valid_access_code()
{
    repo_mysql::FileShareRecord record = share_record();
    record.access_code_hash = infra_storage::sha256_hex("1234");
    service_files::ServiceError error;

    assert(service_files::validate_share_access_candidate(record, "1234", error));
}

static void builds_share_json()
{
    service_files::ShareResult result;
    result.token = "tok\"en";
    result.file_id = 7;
    result.filename = "report.txt";
    result.content_type = "text/plain";
    result.size = 42;
    result.sha256 = "hash";
    result.has_access_code = true;
    result.expires_at = "2026-05-11 14:00:00";
    result.max_downloads = 3;
    result.download_count = 1;

    const std::string body = service_files::build_share_json(result);

    assert(body.find("\"token\":\"tok\\\"en\"") != std::string::npos);
    assert(body.find("\"has_access_code\":true") != std::string::npos);
    assert(body.find("\"max_downloads\":3") != std::string::npos);
    assert(body.find("/api/share/tok\\\"en/download") != std::string::npos);
}

int main()
{
    accepts_valid_share_options();
    rejects_negative_share_limits();
    rejects_long_access_code();
    accepts_public_share_without_access_code();
    rejects_deleted_shared_file();
    rejects_expired_share();
    rejects_download_limit();
    rejects_missing_access_code();
    rejects_invalid_access_code();
    accepts_valid_access_code();
    builds_share_json();
    return 0;
}
