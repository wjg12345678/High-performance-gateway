#include "storage.h"

#include <cerrno>
#include <cstdio>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <unistd.h>

namespace infra_storage
{
std::string storage_root(const std::string &doc_root)
{
    return doc_root + "/uploads";
}

std::string temp_root(const std::string &doc_root)
{
    return storage_root(doc_root) + "/.tmp";
}

std::string file_path(const std::string &doc_root, const std::string &stored_name)
{
    return storage_root(doc_root) + "/" + stored_name;
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

bool file_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

bool file_size(const std::string &path, long &size)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode))
    {
        return false;
    }
    size = static_cast<long>(st.st_size);
    return true;
}

bool remove_file(const std::string &path)
{
    return path.empty() || unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool move_file_or_copy(const std::string &source, const std::string &target)
{
    if (rename(source.c_str(), target.c_str()) == 0)
    {
        return true;
    }

    if (errno != EXDEV)
    {
        return false;
    }

    std::ifstream input(source.c_str(), std::ios::in | std::ios::binary);
    std::ofstream output(target.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!input.is_open() || !output.is_open())
    {
        return false;
    }

    char buffer[65536];
    while (input.good())
    {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0)
        {
            output.write(buffer, count);
        }
    }
    output.close();
    input.close();

    if (!output.good())
    {
        remove_file(target);
        return false;
    }

    remove_file(source);
    return true;
}

bool write_file(const std::string &path, const std::string &content)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        return false;
    }

    output.write(content.data(), content.size());
    output.close();
    if (!output.good())
    {
        remove_file(path);
        return false;
    }
    return true;
}

std::string sha256_hex(const unsigned char *data, size_t len)
{
    static const char kHexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        hex.push_back(kHexChars[(data[i] >> 4) & 0x0F]);
        hex.push_back(kHexChars[data[i] & 0x0F]);
    }
    return hex;
}

std::string sha256_hex(const std::string &content)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(content.data()), content.size(), digest);
    return sha256_hex(digest, sizeof(digest));
}

FileSha256Sink::FileSha256Sink(const std::string &path, size_t max_bytes)
    : m_path(path), m_output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc),
      m_ctx(EVP_MD_CTX_new()), m_size(0), m_max_bytes(max_bytes), m_limit_exceeded(false), m_finished(false)
{
    if (m_ctx != nullptr)
    {
        EVP_DigestInit_ex(m_ctx, EVP_sha256(), nullptr);
    }
}

FileSha256Sink::~FileSha256Sink()
{
    if (m_ctx != nullptr)
    {
        EVP_MD_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
}

bool FileSha256Sink::is_open() const
{
    return m_output.is_open() && m_ctx != nullptr;
}

bool FileSha256Sink::write(const char *data, size_t len)
{
    if (!m_output.is_open())
    {
        return false;
    }
    if (m_size + static_cast<long>(len) > static_cast<long>(m_max_bytes))
    {
        m_limit_exceeded = true;
        return false;
    }
    if (len == 0)
    {
        return true;
    }

    m_output.write(data, len);
    if (!m_output.good())
    {
        return false;
    }
    if (EVP_DigestUpdate(m_ctx, data, len) != 1)
    {
        return false;
    }
    m_size += static_cast<long>(len);
    return true;
}

bool FileSha256Sink::finish()
{
    if (m_finished)
    {
        return true;
    }
    m_output.flush();
    if (!m_output.good())
    {
        return false;
    }
    m_output.close();

    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(m_ctx, digest, &digest_len) != 1)
    {
        return false;
    }
    if (digest_len != SHA256_DIGEST_LENGTH)
    {
        return false;
    }
    m_sha256 = sha256_hex(digest, sizeof(digest));
    m_finished = true;
    return true;
}

long FileSha256Sink::size() const
{
    return m_size;
}

const std::string &FileSha256Sink::sha256() const
{
    return m_sha256;
}

bool FileSha256Sink::limit_exceeded() const
{
    return m_limit_exceeded;
}
}
