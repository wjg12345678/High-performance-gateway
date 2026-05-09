#ifndef HTTP_FILES_MULTIPART_PARSER_H
#define HTTP_FILES_MULTIPART_PARSER_H

#include <cstddef>
#include <map>
#include <string>

namespace http_multipart
{
struct UploadedFile
{
    std::string temp_path;
    std::string filename;
    std::string content_type;
    std::string sha256;
    long size;

    UploadedFile() : size(0) {}
};

struct ParseResult
{
    std::map<std::string, std::string> fields;
    UploadedFile file;
    bool has_file;

    ParseResult() : has_file(false) {}
};

struct ParseError
{
    int status;
    std::string title;
    std::string message;

    ParseError() : status(500), title("Internal Error") {}
};

bool parse_spooled_multipart(const std::string &spool_path, const std::string &content_type,
                             const std::string &doc_root, size_t upload_max_bytes,
                             ParseResult &result, ParseError &error);
}

#endif
