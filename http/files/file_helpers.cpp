#include "file_helpers.h"

#include <cctype>
#include <sys/stat.h>

namespace http_file_helpers
{
std::string sanitize_filename(const std::string &value)
{
    std::string cleaned;
    cleaned.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (isalnum(ch) || ch == '.' || ch == '_' || ch == '-')
        {
            cleaned.push_back(static_cast<char>(ch));
        }
        else if (ch == ' ')
        {
            cleaned.push_back('_');
        }
    }

    if (cleaned.empty() || cleaned == "." || cleaned == "..")
    {
        cleaned = "file.txt";
    }
    if (cleaned.size() > 80)
    {
        cleaned.resize(80);
    }
    return cleaned;
}

std::string file_storage_root(const std::string &doc_root)
{
    return doc_root + "/uploads";
}

std::string build_file_disk_path(const std::string &doc_root, const std::string &stored_name)
{
    return file_storage_root(doc_root) + "/" + stored_name;
}

bool ensure_directory(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}
}
