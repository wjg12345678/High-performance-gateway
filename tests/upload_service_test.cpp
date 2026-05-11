#include <cassert>
#include <string>

#include "../service/files/upload_service.h"

static service_files::UploadQuota quota(long max_single_file_bytes, long max_total_bytes)
{
    service_files::UploadQuota value;
    value.max_single_file_bytes = max_single_file_bytes;
    value.max_total_bytes = max_total_bytes;
    return value;
}

static void rejects_negative_upload_size()
{
    service_files::UploadQuotaStatus status;
    service_files::ServiceError error;

    const bool ok = service_files::evaluate_upload_quota(-1, 99, quota(100, 1000), status, error);

    assert(!ok);
    assert(!status.allowed);
    assert(status.requested_bytes == -1);
    assert(status.used_bytes == 99);
    assert(status.reason == "invalid file size");
    assert(error.code == service_files::ErrorCode::InvalidArgument);
}

static void allows_upload_within_limits()
{
    service_files::UploadQuotaStatus status;
    service_files::ServiceError error;

    const bool ok = service_files::evaluate_upload_quota(40, 100, quota(100, 1000), status, error);

    assert(ok);
    assert(status.allowed);
    assert(status.requested_bytes == 40);
    assert(status.used_bytes == 100);
    assert(status.remaining_bytes == 900);
    assert(status.reason.empty());
}

static void rejects_single_file_limit()
{
    service_files::UploadQuotaStatus status;
    service_files::ServiceError error;

    const bool ok = service_files::evaluate_upload_quota(101, 200, quota(100, 1000), status, error);

    assert(ok);
    assert(!status.allowed);
    assert(status.remaining_bytes == 800);
    assert(status.reason == "file exceeds single file limit of 100 bytes");
    assert(error.code == service_files::ErrorCode::PayloadTooLarge);
}

static void rejects_total_quota_limit()
{
    service_files::UploadQuotaStatus status;
    service_files::ServiceError error;

    const bool ok = service_files::evaluate_upload_quota(50, 75, quota(0, 100), status, error);

    assert(ok);
    assert(!status.allowed);
    assert(status.remaining_bytes == 25);
    assert(status.reason == "user storage quota exceeded");
    assert(error.code == service_files::ErrorCode::Conflict);
}

static void treats_zero_total_quota_as_unlimited()
{
    service_files::UploadQuotaStatus status;
    service_files::ServiceError error;

    const bool ok = service_files::evaluate_upload_quota(500, 900, quota(0, 0), status, error);

    assert(ok);
    assert(status.allowed);
    assert(status.remaining_bytes == 0);
    assert(status.max_total_bytes == 0);
}

static void builds_preflight_json()
{
    service_files::UploadQuotaStatus status;
    status.requested_bytes = 101;
    status.used_bytes = 20;
    status.remaining_bytes = 80;
    status.max_single_file_bytes = 100;
    status.max_total_bytes = 100;
    status.allowed = false;
    status.reason = "quote \"inside\"";

    const std::string body = service_files::build_upload_preflight_json(status);

    assert(body.find("\"allowed\":false") != std::string::npos);
    assert(body.find("\"reason\":\"quote \\\"inside\\\"\"") != std::string::npos);
}

static void builds_upload_success_json()
{
    service_files::UploadResult result;
    result.file_id = 7;
    result.folder_id = 2;
    result.physical_id = 3;
    result.filename = "report \"final\".txt";
    result.size = 42;
    result.is_public = true;
    result.deduplicated = true;
    result.sha256 = "abc123";

    const std::string body = service_files::build_upload_success_json(result);

    assert(body.find("\"id\":7") != std::string::npos);
    assert(body.find("\"filename\":\"report \\\"final\\\".txt\"") != std::string::npos);
    assert(body.find("\"is_public\":true") != std::string::npos);
    assert(body.find("\"deduplicated\":true") != std::string::npos);
}

int main()
{
    rejects_negative_upload_size();
    allows_upload_within_limits();
    rejects_single_file_limit();
    rejects_total_quota_limit();
    treats_zero_total_quota_as_unlimited();
    builds_preflight_json();
    builds_upload_success_json();
    return 0;
}
