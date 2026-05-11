#include "upload_service.h"

#include "../../http/files/file_helpers.h"
#include "../../infra/storage/storage.h"
#include "../../repo/mysql/mysql_utils.h"

#include <cstddef>
#include <cstdlib>

namespace
{
const size_t kStoredNamePrefixLength = 24;

void set_not_found(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::NotFound;
    error.message = message;
}

void set_conflict(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::Conflict;
    error.message = message;
}

void set_bad_request(service_files::ServiceError &error, const char *message)
{
    error.code = service_files::ErrorCode::InvalidArgument;
    error.message = message;
}

void set_payload_too_large(service_files::ServiceError &error, const std::string &message)
{
    error.code = service_files::ErrorCode::PayloadTooLarge;
    error.message = message;
}

class MysqlTransaction
{
public:
    explicit MysqlTransaction(MYSQL *mysql) : m_mysql(mysql), m_active(false), m_committed(false) {}

    ~MysqlTransaction()
    {
        if (m_active && !m_committed)
        {
            repo_mysql::rollback_transaction(m_mysql);
        }
    }

    bool begin()
    {
        if (!repo_mysql::begin_transaction(m_mysql))
        {
            return false;
        }
        m_active = true;
        return true;
    }

    bool commit()
    {
        if (!m_active)
        {
            return false;
        }
        if (!repo_mysql::commit_transaction(m_mysql))
        {
            return false;
        }
        m_committed = true;
        m_active = false;
        return true;
    }

private:
    MYSQL *m_mysql;
    bool m_active;
    bool m_committed;
};

bool materialize_upload_payload(const service_files::UploadPayload &payload, const std::string &disk_path)
{
    if (payload.use_temp_file)
    {
        return infra_storage::move_file_or_copy(payload.temp_path, disk_path);
    }
    return infra_storage::write_file(disk_path, payload.content);
}

bool env_flag_enabled(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}
}

namespace service_files
{
std::string build_upload_success_json(const UploadResult &result)
{
    std::string body = "{\"code\":0,\"message\":\"upload success\",\"file\":{\"id\":";
    body += std::to_string(result.file_id);
    body += ",\"filename\":\"";
    body += json_escape(result.filename);
    body += "\",\"folder_id\":";
    body += std::to_string(result.folder_id);
    body += ",\"physical_id\":";
    body += std::to_string(result.physical_id);
    body += ",\"size\":";
    body += std::to_string(result.size);
    body += ",\"is_public\":";
    body += result.is_public ? "true" : "false";
    body += ",\"deduplicated\":";
    body += result.deduplicated ? "true" : "false";
    body += ",\"sha256\":\"";
    body += json_escape(result.sha256);
    body += "\"}}";
    return body;
}

std::string build_upload_preflight_json(const UploadQuotaStatus &status)
{
    std::string body = "{\"code\":0,\"allowed\":";
    body += status.allowed ? "true" : "false";
    body += ",\"requested_bytes\":";
    body += std::to_string(status.requested_bytes);
    body += ",\"used_bytes\":";
    body += std::to_string(status.used_bytes);
    body += ",\"remaining_bytes\":";
    body += std::to_string(status.remaining_bytes);
    body += ",\"max_single_file_bytes\":";
    body += std::to_string(status.max_single_file_bytes);
    body += ",\"max_total_bytes\":";
    body += std::to_string(status.max_total_bytes);
    body += ",\"reason\":";
    if (status.reason.empty())
    {
        body += "null";
    }
    else
    {
        body += "\"";
        body += json_escape(status.reason);
        body += "\"";
    }
    body += "}";
    return body;
}

bool evaluate_upload_quota(long requested_bytes, long used_bytes, const UploadQuota &quota,
                           UploadQuotaStatus &status, ServiceError &error)
{
    status = UploadQuotaStatus();
    status.requested_bytes = requested_bytes;
    status.max_single_file_bytes = quota.max_single_file_bytes;
    status.max_total_bytes = quota.max_total_bytes;
    status.used_bytes = used_bytes;

    if (requested_bytes < 0)
    {
        set_bad_request(error, "invalid file size");
        status.reason = error.message;
        return false;
    }

    status.remaining_bytes = quota.max_total_bytes > 0 && used_bytes < quota.max_total_bytes
                                 ? quota.max_total_bytes - used_bytes
                                 : 0;
    if (quota.max_total_bytes <= 0)
    {
        status.remaining_bytes = 0;
    }

    if (quota.max_single_file_bytes > 0 && requested_bytes > quota.max_single_file_bytes)
    {
        status.reason = "file exceeds single file limit of " + std::to_string(quota.max_single_file_bytes) + " bytes";
        set_payload_too_large(error, status.reason);
        return true;
    }

    if (quota.max_total_bytes > 0 && requested_bytes > status.remaining_bytes)
    {
        status.reason = "user storage quota exceeded";
        set_conflict(error, status.reason.c_str());
        return true;
    }

    status.allowed = true;
    return true;
}

bool inspect_upload_quota(MYSQL *mysql, const std::string &owner, long requested_bytes,
                          const UploadQuota &quota, UploadQuotaStatus &status, ServiceError &error)
{
    if (requested_bytes < 0)
    {
        return evaluate_upload_quota(requested_bytes, 0, quota, status, error);
    }

    long used_bytes = 0;
    if (!repo_mysql::fetch_user_storage_usage(mysql, owner, used_bytes))
    {
        return false;
    }

    return evaluate_upload_quota(requested_bytes, used_bytes, quota, status, error);
}

bool create_uploaded_file(MYSQL *mysql, const std::string &doc_root, const std::string &owner,
                          const std::string &stored_name_prefix, UploadPayload &payload,
                          bool is_public, const UploadQuota &quota, UploadResult &result, ServiceError *error)
{
    if (!infra_storage::ensure_directory(infra_storage::storage_root(doc_root)))
    {
        return false;
    }

    if (payload.sha256.empty() && !payload.content.empty())
    {
        payload.sha256 = infra_storage::sha256_hex(payload.content);
    }
    if (payload.sha256.empty())
    {
        return false;
    }

    MysqlTransaction transaction(mysql);
    if (!transaction.begin())
    {
        return false;
    }

    if (!repo_mysql::lock_user_for_update(mysql, owner))
    {
        return false;
    }

    ServiceError quota_error;
    UploadQuotaStatus quota_status;
    if (!inspect_upload_quota(mysql, owner, payload.size, quota, quota_status, quota_error))
    {
        return false;
    }
    if (!quota_status.allowed)
    {
        if (error != nullptr)
        {
            *error = quota_error;
        }
        return false;
    }

    bool folder_exists = false;
    if (!repo_mysql::folder_exists(mysql, owner, payload.folder_id, folder_exists))
    {
        return false;
    }
    if (!folder_exists)
    {
        if (error != nullptr)
        {
            set_not_found(*error, "folder not found");
        }
        return false;
    }

    payload.filename = ensure_unique_owned_filename(mysql, owner, payload.filename, payload.folder_id);

    repo_mysql::PhysicalFileRecord physical;
    bool deduplicated = false;
    bool physical_created = false;
    std::string new_physical_disk_path;
    const bool has_physical = repo_mysql::fetch_physical_file_by_sha256_for_update(mysql, payload.sha256, physical);
    if (has_physical)
    {
        const std::string disk_path = infra_storage::file_path(doc_root, physical.stored_name);
        if (infra_storage::file_exists(disk_path))
        {
            deduplicated = true;
            if (payload.use_temp_file)
            {
                infra_storage::remove_file(payload.temp_path);
            }
        }
        else if (payload.use_temp_file)
        {
            if (!infra_storage::move_file_or_copy(payload.temp_path, disk_path))
            {
                return false;
            }
        }
        else if (!infra_storage::write_file(disk_path, payload.content))
        {
            return false;
        }
    }
    else
    {
        physical.stored_name = stored_name_prefix.substr(0, kStoredNamePrefixLength);
        if (!payload.sha256.empty())
        {
            physical.stored_name += "_" + payload.sha256.substr(0, 12);
        }
        physical.stored_name += http_file_helpers::file_extension(payload.filename);
        physical.sha256 = payload.sha256;
        physical.file_size = payload.size;
        physical.ref_count = 0;

        long physical_id = 0;
        if (!repo_mysql::insert_physical_file(mysql, physical, physical_id))
        {
            if (repo_mysql::last_errno(mysql) == 1062 &&
                repo_mysql::fetch_physical_file_by_sha256_for_update(mysql, payload.sha256, physical))
            {
                const std::string existing_path = infra_storage::file_path(doc_root, physical.stored_name);
                if (infra_storage::file_exists(existing_path))
                {
                    deduplicated = true;
                    if (payload.use_temp_file)
                    {
                        infra_storage::remove_file(payload.temp_path);
                    }
                }
                else if (!materialize_upload_payload(payload, existing_path))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
        else
        {
            physical.id = physical_id;
            physical_created = true;
            new_physical_disk_path = infra_storage::file_path(doc_root, physical.stored_name);
            if (!materialize_upload_payload(payload, new_physical_disk_path))
            {
                return false;
            }
        }
    }

    repo_mysql::FileCreateRecord file_record;
    file_record.owner = owner;
    file_record.stored_name = physical.stored_name;
    file_record.physical_id = physical.id;
    file_record.original_name = payload.filename;
    file_record.content_type = payload.content_type;
    file_record.folder_id = payload.folder_id;
    file_record.file_size = payload.size;
    file_record.is_public = is_public;
    file_record.sha256 = payload.sha256;

    long file_id = 0;
    if (!repo_mysql::insert_file(mysql, file_record, file_id))
    {
        if (repo_mysql::last_errno(mysql) == 1062 && error != nullptr)
        {
            set_conflict(*error, "file name already exists");
        }
        if (physical_created)
        {
            infra_storage::remove_file(new_physical_disk_path);
        }
        return false;
    }

    if (physical_created && env_flag_enabled("TWS_TEST_FAIL_UPLOAD_BEFORE_COMMIT"))
    {
        if (error != nullptr)
        {
            set_conflict(*error, "test failpoint before upload commit");
        }
        infra_storage::remove_file(new_physical_disk_path);
        return false;
    }

    if (!transaction.commit())
    {
        return false;
    }

    result.file_id = file_id;
    result.folder_id = payload.folder_id;
    result.physical_id = physical.id;
    result.filename = payload.filename;
    result.size = payload.size;
    result.is_public = is_public;
    result.deduplicated = deduplicated;
    result.sha256 = payload.sha256;
    return true;
}
}
