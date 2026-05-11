#include <cassert>
#include <string>

#include "../http/files/file_helpers.h"

static void sanitizes_upload_filenames()
{
    assert(http_file_helpers::sanitize_filename("../../secret.txt") == "....secret.txt");
    assert(http_file_helpers::sanitize_filename("  report   final .pdf  ") == "report final .pdf");
    assert(http_file_helpers::sanitize_filename(".") == "file.txt");
    assert(http_file_helpers::sanitize_filename("..") == "file.txt");

    const std::string long_name(120, 'a');
    assert(http_file_helpers::sanitize_filename(long_name).size() == 80);
}

static void sanitizes_download_filenames()
{
    assert(http_file_helpers::sanitize_download_filename("a/b;c\"d.txt") == "abcd.txt");
    assert(http_file_helpers::sanitize_download_filename("report final.pdf") == "report final.pdf");
    assert(http_file_helpers::sanitize_download_filename("\n") == "_");
}

static void encodes_download_filenames()
{
    assert(http_file_helpers::encode_download_filename("atlas report.txt") == "atlas%20report.txt");
    assert(http_file_helpers::encode_download_filename("a+b.txt") == "a%2Bb.txt");
    assert(http_file_helpers::encode_download_filename("") == "download");
}

static void derives_paths_and_extensions()
{
    assert(http_file_helpers::file_storage_root("/srv/www") == "/srv/www/uploads");
    assert(http_file_helpers::temp_storage_root("/srv/www") == "/srv/www/uploads/.tmp");
    assert(http_file_helpers::build_file_disk_path("/srv/www", "abc.bin") == "/srv/www/uploads/abc.bin");
    assert(http_file_helpers::file_extension("archive.tar.gz") == ".gz");
    assert(http_file_helpers::file_extension("/tmp/no_extension") == "");
    assert(http_file_helpers::file_extension("/tmp/.hidden") == ".hidden");
}

int main()
{
    sanitizes_upload_filenames();
    sanitizes_download_filenames();
    encodes_download_filenames();
    derives_paths_and_extensions();
    return 0;
}
