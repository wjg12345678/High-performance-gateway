#ifndef SERVICE_FILES_UPLOAD_SERVICE_H
#define SERVICE_FILES_UPLOAD_SERVICE_H

#include "file_service.h"

#include <string>

namespace service_files
{
struct UploadPayload
{
    std::string filename;
    std::string content;
    std::string content_type;
    std::string temp_path;
    std::string sha256;
    long folder_id;
    long size;
    bool use_temp_file;

    UploadPayload() : folder_id(0), size(0), use_temp_file(false) {}
};

struct UploadResult
{
    long file_id;
    long folder_id;
    long physical_id;
    std::string filename;
    long size;
    bool is_public;
    bool deduplicated;
    std::string sha256;

    UploadResult() : file_id(0), folder_id(0), physical_id(0), size(0), is_public(false), deduplicated(false) {}
};

struct UploadQuota
{
    long max_single_file_bytes;
    long max_total_bytes;

    UploadQuota() : max_single_file_bytes(0), max_total_bytes(0) {}
};

struct UploadQuotaStatus
{
    long requested_bytes;
    long used_bytes;
    long remaining_bytes;
    long max_single_file_bytes;
    long max_total_bytes;
    bool allowed;
    std::string reason;

    UploadQuotaStatus()
        : requested_bytes(0), used_bytes(0), remaining_bytes(0), max_single_file_bytes(0),
          max_total_bytes(0), allowed(false) {}
};

std::string build_upload_success_json(const UploadResult &result);
std::string build_upload_preflight_json(const UploadQuotaStatus &status);

bool evaluate_upload_quota(long requested_bytes, long used_bytes, const UploadQuota &quota,
                           UploadQuotaStatus &status, ServiceError &error);
bool inspect_upload_quota(MYSQL *mysql, const std::string &owner, long requested_bytes,
                          const UploadQuota &quota, UploadQuotaStatus &status, ServiceError &error);
bool create_uploaded_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner,
                          const std::string &stored_name_prefix, UploadPayload &payload,
                          bool is_public, const UploadQuota &quota, UploadResult &result,
                          ServiceError *error = nullptr);
}

#endif
