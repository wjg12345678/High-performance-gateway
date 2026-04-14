#include "file_helpers.h"

#include <cctype>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

namespace http_file_helpers
{
std::string sanitize_filename(const std::string &value)
{
    std::string cleaned;
    cleaned.reserve(value.size());
    bool previous_space = false;
    for (size_t i = 0; i < value.size();)
    {
        unsigned char ch = static_cast<unsigned char>(value[i]);

        if (ch < 0x80)
        {
            if (std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '(' || ch == ')' || ch == '[' || ch == ']')
            {
                cleaned.push_back(static_cast<char>(ch));
                previous_space = false;
            }
            else if (std::isspace(ch))
            {
                if (!cleaned.empty() && !previous_space)
                {
                    cleaned.push_back(' ');
                    previous_space = true;
                }
            }
            i += 1;
            continue;
        }

        if ((ch & 0xE0) == 0xC0 && i + 1 < value.size())
        {
            cleaned.append(value, i, 2);
            i += 2;
            previous_space = false;
            continue;
        }
        if ((ch & 0xF0) == 0xE0 && i + 2 < value.size())
        {
            cleaned.append(value, i, 3);
            i += 3;
            previous_space = false;
            continue;
        }
        if ((ch & 0xF8) == 0xF0 && i + 3 < value.size())
        {
            cleaned.append(value, i, 4);
            i += 4;
            previous_space = false;
            continue;
        }

        i += 1;
    }

    while (!cleaned.empty() && cleaned.back() == ' ')
    {
        cleaned.pop_back();
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

std::string sanitize_download_filename(const std::string &value)
{
    std::string cleaned;
    cleaned.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch >= 0x20 && ch < 0x7F && ch != '"' && ch != '\\' && ch != '/' && ch != ';')
        {
            cleaned.push_back(static_cast<char>(ch));
        }
        else if (std::isspace(ch))
        {
            cleaned.push_back('_');
        }
    }

    if (cleaned.empty() || cleaned == "." || cleaned == "..")
    {
        return "download.txt";
    }
    if (cleaned.size() > 120)
    {
        cleaned.resize(120);
    }
    return cleaned;
}

std::string encode_download_filename(const std::string &value)
{
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (size_t i = 0; i < value.size(); ++i)
    {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' || ch == '~')
        {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }

        char buffer[4];
        snprintf(buffer, sizeof(buffer), "%%%02X", ch);
        encoded += buffer;
    }
    return encoded.empty() ? "download.txt" : encoded;
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
