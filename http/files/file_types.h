#ifndef HTTP_FILE_TYPES_H
#define HTTP_FILE_TYPES_H

#include <string>

struct ManagedFileRecord
{
    long file_id;
    long file_size;
    bool is_public;
    bool is_deleted;
    std::string owner;
    std::string stored_name;
    std::string original_name;
    std::string content_type;
    std::string content_sha256;
    std::string deleted_at;

    ManagedFileRecord() : file_id(0), file_size(0), is_public(false), is_deleted(false) {}
};

#endif
