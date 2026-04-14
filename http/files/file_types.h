#ifndef HTTP_FILE_TYPES_H
#define HTTP_FILE_TYPES_H

#include <string>

struct ManagedFileRecord
{
    long file_id;
    long file_size;
    std::string owner;
    std::string stored_name;
    std::string original_name;
    std::string content_type;

    ManagedFileRecord() : file_id(0), file_size(0) {}
};

#endif
