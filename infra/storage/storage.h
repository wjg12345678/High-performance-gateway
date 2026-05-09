#ifndef INFRA_STORAGE_STORAGE_H
#define INFRA_STORAGE_STORAGE_H

#include <cstddef>
#include <fstream>
#include <openssl/evp.h>
#include <string>

namespace infra_storage
{
std::string storage_root(const std::string &doc_root);
std::string temp_root(const std::string &doc_root);
std::string file_path(const std::string &doc_root, const std::string &stored_name);

bool ensure_directory(const std::string &path);
bool file_exists(const std::string &path);
bool file_size(const std::string &path, long &size);
bool remove_file(const std::string &path);
bool move_file_or_copy(const std::string &source, const std::string &target);
bool write_file(const std::string &path, const std::string &content);

std::string sha256_hex(const unsigned char *data, size_t len);
std::string sha256_hex(const std::string &content);

class FileSha256Sink
{
public:
    FileSha256Sink(const std::string &path, size_t max_bytes);
    ~FileSha256Sink();

    bool is_open() const;
    bool write(const char *data, size_t len);
    bool finish();

    long size() const;
    const std::string &sha256() const;
    bool limit_exceeded() const;

private:
    std::string m_path;
    std::ofstream m_output;
    EVP_MD_CTX *m_ctx;
    long m_size;
    size_t m_max_bytes;
    bool m_limit_exceeded;
    bool m_finished;
    std::string m_sha256;
};
}

#endif
