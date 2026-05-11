#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../http/files/multipart_parser.h"
#include "../infra/storage/storage.h"

namespace
{
std::string make_temp_dir()
{
    char pattern[] = "/tmp/atlas-multipart-test-XXXXXX";
    char *path = mkdtemp(pattern);
    assert(path != nullptr);
    return path;
}

void write_text_file(const std::string &path, const std::string &content)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    assert(output.is_open());
    output.write(content.data(), content.size());
    output.close();
    assert(output.good());
}

bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

void cleanup_doc_root(const std::string &doc_root)
{
    std::remove((doc_root + "/uploads/.tmp/spool.txt").c_str());
    std::remove((doc_root + "/uploads/.tmp/spool-big.txt").c_str());
    rmdir((doc_root + "/uploads/.tmp").c_str());
    rmdir((doc_root + "/uploads").c_str());
    rmdir(doc_root.c_str());
}

std::string multipart_body(const std::string &boundary, const std::string &file_content)
{
    return "--" + boundary + "\r\n"
           "Content-Disposition: form-data; name=\"folder_id\"\r\n\r\n"
           "7\r\n"
           "--" + boundary + "\r\n"
           "Content-Disposition: form-data; name=\"file\"; filename=\"../atlas report.txt\"\r\n"
           "Content-Type: text/plain\r\n\r\n" +
           file_content + "\r\n"
           "--" + boundary + "--\r\n";
}
}

static void parses_spooled_multipart_upload()
{
    const std::string doc_root = make_temp_dir();
    assert(infra_storage::ensure_directory(doc_root + "/uploads"));
    assert(infra_storage::ensure_directory(doc_root + "/uploads/.tmp"));

    const std::string spool_path = doc_root + "/uploads/.tmp/spool.txt";
    write_text_file(spool_path, multipart_body("atlas-boundary", "hello atlas"));

    http_multipart::ParseResult result;
    http_multipart::ParseError error;
    assert(http_multipart::parse_spooled_multipart(
        spool_path,
        "multipart/form-data; boundary=atlas-boundary",
        doc_root,
        1024,
        result,
        error));

    assert(result.has_file);
    assert(result.fields["folder_id"] == "7");
    assert(result.fields["file"] == "..atlas report.txt");
    assert(result.file.filename == "..atlas report.txt");
    assert(result.file.content_type == "text/plain");
    assert(result.file.size == 11);
    assert(result.file.sha256 == infra_storage::sha256_hex("hello atlas"));
    assert(exists(result.file.temp_path));

    std::remove(result.file.temp_path.c_str());
    cleanup_doc_root(doc_root);
}

static void rejects_uploads_over_file_limit()
{
    const std::string doc_root = make_temp_dir();
    assert(infra_storage::ensure_directory(doc_root + "/uploads"));
    assert(infra_storage::ensure_directory(doc_root + "/uploads/.tmp"));

    const std::string spool_path = doc_root + "/uploads/.tmp/spool-big.txt";
    write_text_file(spool_path, multipart_body("atlas-boundary", "hello atlas"));

    http_multipart::ParseResult result;
    http_multipart::ParseError error;
    assert(!http_multipart::parse_spooled_multipart(
        spool_path,
        "multipart/form-data; boundary=atlas-boundary",
        doc_root,
        5,
        result,
        error));

    assert(error.status == 413);
    assert(!result.has_file);
    cleanup_doc_root(doc_root);
}

int main()
{
    parses_spooled_multipart_upload();
    rejects_uploads_over_file_limit();
    return 0;
}
