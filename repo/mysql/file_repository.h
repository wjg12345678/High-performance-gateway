#ifndef REPO_MYSQL_FILE_REPOSITORY_H
#define REPO_MYSQL_FILE_REPOSITORY_H

#include <mysql/mysql.h>
#include <string>
#include <vector>

#include "../../http/files/file_types.h"

namespace repo_mysql
{
struct FileCreateRecord
{
    std::string owner;
    std::string stored_name;
    std::string original_name;
    std::string content_type;
    long physical_id;
    long folder_id;
    long file_size;
    bool is_public;
    std::string sha256;

    FileCreateRecord() : physical_id(0), folder_id(0), file_size(0), is_public(false) {}
};

struct PhysicalFileRecord
{
    long id;
    long file_size;
    long ref_count;
    std::string sha256;
    std::string stored_name;

    PhysicalFileRecord() : id(0), file_size(0), ref_count(0) {}
};

struct FolderCreateRecord
{
    std::string owner;
    long parent_id;
    std::string name;

    FolderCreateRecord() : parent_id(0) {}
};

struct FolderListItem
{
    long id;
    long parent_id;
    std::string name;
    std::string created_at;

    FolderListItem() : id(0), parent_id(0) {}
};

struct FileListItem
{
    long id;
    long folder_id;
    long size;
    bool is_public;
    std::string filename;
    std::string content_type;
    std::string owner;
    std::string created_at;
    std::string deleted_at;

    FileListItem() : id(0), folder_id(0), size(0), is_public(false) {}
};


struct FileShareCreateRecord
{
    std::string token;
    long file_id;
    std::string owner;
    std::string access_code_hash;
    long expires_in_seconds;
    long max_downloads;

    FileShareCreateRecord() : file_id(0), expires_in_seconds(0), max_downloads(0) {}
};

struct FileShareRecord
{
    std::string token;
    long file_id;
    std::string owner;
    std::string access_code_hash;
    std::string expires_at;
    long max_downloads;
    long download_count;
    bool is_expired;
    ManagedFileRecord file;

    FileShareRecord() : file_id(0), max_downloads(0), download_count(0), is_expired(false) {}
};

struct PageIds
{
    std::vector<long> ids;
    long next_cursor;

    PageIds() : next_cursor(0) {}
};

bool ensure_file_shares_table(MYSQL *mysql);
bool insert_file_share(MYSQL *mysql, const FileShareCreateRecord &record);
bool fetch_file_share(MYSQL *mysql, const std::string &token, FileShareRecord &record);
bool increment_file_share_download_count(MYSQL *mysql, const std::string &token);
bool fetch_file_record(MYSQL *mysql, long file_id, ManagedFileRecord &record, bool include_deleted = false);
unsigned int last_errno(MYSQL *mysql);
bool fetch_physical_file_by_sha256(MYSQL *mysql, const std::string &sha256, PhysicalFileRecord &record);
bool insert_physical_file(MYSQL *mysql, const PhysicalFileRecord &record, long &physical_id);
bool increment_physical_ref(MYSQL *mysql, long physical_id);
bool decrement_physical_ref(MYSQL *mysql, long physical_id);
bool delete_physical_file_if_unreferenced(MYSQL *mysql, long physical_id);
bool original_name_exists(MYSQL *mysql, const std::string &owner, const std::string &original_name,
                          long folder_id, long ignore_file_id, bool &exists);
bool insert_file(MYSQL *mysql, const FileCreateRecord &record, long &file_id);
bool fetch_user_storage_usage(MYSQL *mysql, const std::string &owner, long &used_bytes);
bool folder_exists(MYSQL *mysql, const std::string &owner, long folder_id, bool &exists);
bool folder_name_exists(MYSQL *mysql, const std::string &owner, long parent_id,
                        const std::string &name, bool &exists);
bool insert_folder(MYSQL *mysql, const FolderCreateRecord &record, long &folder_id);
bool fetch_folder(MYSQL *mysql, const std::string &owner, long folder_id, FolderListItem &item);
bool folder_has_active_children(MYSQL *mysql, const std::string &owner, long folder_id, bool &has_children);
bool soft_delete_folder(MYSQL *mysql, const std::string &owner, long folder_id);
bool fetch_drive_folders(MYSQL *mysql, const std::string &owner, long parent_id,
                         std::vector<FolderListItem> &items);
bool fetch_drive_files(MYSQL *mysql, const std::string &owner, long folder_id,
                       std::vector<FileListItem> &items);
bool fetch_private_file_page_ids(MYSQL *mysql, const std::string &owner, bool include_deleted,
                                 long cursor, long limit, PageIds &page);
bool fetch_public_file_page_ids(MYSQL *mysql, long cursor, long limit, PageIds &page);
bool fetch_private_file_list(MYSQL *mysql, const std::vector<long> &ids, std::vector<FileListItem> &items);
bool fetch_public_file_list(MYSQL *mysql, const std::vector<long> &ids, std::vector<FileListItem> &items);
bool soft_delete_file(MYSQL *mysql, long file_id);
bool update_file_visibility(MYSQL *mysql, long file_id, bool is_public);
bool restore_file(MYSQL *mysql, long file_id, const std::string &restored_name);
}

#endif
