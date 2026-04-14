#ifndef HTTP_FILE_HELPERS_H
#define HTTP_FILE_HELPERS_H

#include <string>

namespace http_file_helpers
{
std::string sanitize_filename(const std::string &value);
std::string file_storage_root(const std::string &doc_root);
std::string build_file_disk_path(const std::string &doc_root, const std::string &stored_name);
bool ensure_directory(const std::string &path);
}

#endif
