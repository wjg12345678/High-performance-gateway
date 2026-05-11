#include <cassert>
#include <string>

#include "../service/files/file_service.h"

static ManagedFileRecord hard_delete_record(const std::string &owner, bool is_deleted)
{
    ManagedFileRecord record;
    record.file_id = 7;
    record.owner = owner;
    record.original_name = "demo.txt";
    record.stored_name = "stored.txt";
    record.is_deleted = is_deleted;
    return record;
}

static void allows_deleted_owner_file_for_permanent_delete()
{
    service_files::ServiceError error;
    const ManagedFileRecord record = hard_delete_record("alice", true);

    assert(service_files::validate_hard_delete_candidate(record, "alice", error));
}

static void rejects_permanent_delete_by_non_owner()
{
    service_files::ServiceError error;
    const ManagedFileRecord record = hard_delete_record("alice", true);

    assert(!service_files::validate_hard_delete_candidate(record, "bob", error));
    assert(error.code == service_files::ErrorCode::Forbidden);
    assert(error.message == "forbidden");
}

static void rejects_permanent_delete_before_recycle_bin()
{
    service_files::ServiceError error;
    const ManagedFileRecord record = hard_delete_record("alice", false);

    assert(!service_files::validate_hard_delete_candidate(record, "alice", error));
    assert(error.code == service_files::ErrorCode::Conflict);
    assert(error.message == "file must be moved to recycle bin before permanent deletion");
}

static void builds_restore_json()
{
    ManagedFileRecord record = hard_delete_record("alice", false);
    record.folder_id = 3;
    record.content_type = "text/plain";
    record.file_size = 42;
    record.is_public = false;

    const std::string body = service_files::build_file_restored_json(record);

    assert(body.find("\"message\":\"file restored\"") != std::string::npos);
    assert(body.find("\"id\":7") != std::string::npos);
    assert(body.find("\"folder_id\":3") != std::string::npos);
    assert(body.find("\"filename\":\"demo.txt\"") != std::string::npos);
}

int main()
{
    allows_deleted_owner_file_for_permanent_delete();
    rejects_permanent_delete_by_non_owner();
    rejects_permanent_delete_before_recycle_bin();
    builds_restore_json();
    return 0;
}
