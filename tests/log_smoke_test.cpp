#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "../infra/log/log.h"

namespace
{
std::string current_log_path(const std::string &base_path)
{
    time_t now = time(NULL);
    tm time_info;
    localtime_r(&now, &time_info);

    char prefix[64] = {0};
    snprintf(prefix, sizeof(prefix), "%d_%02d_%02d_",
             time_info.tm_year + 1900, time_info.tm_mon + 1, time_info.tm_mday);
    return std::string(base_path).insert(base_path.find_last_of('/') + 1, prefix);
}

std::string read_file(const std::string &path)
{
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    return 1;
}
}

int main()
{
    char dir_template[] = "/tmp/atlas-log-smoke-XXXXXX";
    char *dir = mkdtemp(dir_template);
    if (dir == NULL)
    {
        return fail("mkdtemp failed");
    }

    const std::string base_path = std::string(dir) + "/ServerLog";
    if (!Log::get_instance()->init(base_path.c_str(), 0, 1024, 1000, 16, Log::DEBUG))
    {
        rmdir(dir);
        return fail("log init failed");
    }

    for (int i = 0; i < 100; ++i)
    {
        Log::get_instance()->write_log(Log::INFO, "async smoke line=%d", i);
    }
    Log::get_instance()->flush();

    const std::string log_path = current_log_path(base_path);
    const std::string content = read_file(log_path);
    if (content.find("async smoke line=0") == std::string::npos ||
        content.find("async smoke line=99") == std::string::npos)
    {
        remove(log_path.c_str());
        rmdir(dir);
        return fail("expected log content not found");
    }

    remove(log_path.c_str());
    rmdir(dir);
    return 0;
}
