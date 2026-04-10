#include "config.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
string trim(const string &input)
{
    size_t start = input.find_first_not_of(" \t\r\n");
    if (start == string::npos)
    {
        return "";
    }
    size_t end = input.find_last_not_of(" \t\r\n");
    return input.substr(start, end - start + 1);
}

const char *getenv_value(const char *name)
{
    const char *value = getenv(name);
    return (value != NULL && value[0] != '\0') ? value : NULL;
}

int getenv_int_value(const char *name, int fallback)
{
    const char *value = getenv_value(name);
    return value ? atoi(value) : fallback;
}
}

Config::Config(){
    //端口号,默认9006
    PORT = 9006;

    //日志写入方式，默认同步
    LOGWrite = 0;

    log_level = 1;
    log_split_lines = 800000;
    log_queue_size = 800;

    //触发组合模式,默认listenfd ET + connfd ET
    TRIGMode = 3;

    //listenfd触发模式，默认ET
    LISTENTrigmode = 1;

    //connfd触发模式，默认ET
    CONNTrigmode = 1;

    //优雅关闭链接，默认不使用
    OPT_LINGER = 0;

    //数据库连接池数量,默认8
    sql_num = 8;

    //线程池内的线程数量,默认8
    thread_num = 8;

    //关闭日志,默认不关闭
    close_log = 0;

    //并发模型,默认是proactor
    actor_model = 0;

    //守护进程模式，默认前台运行
    daemon_mode = 0;
    pid_file = "./atlas-webserver.pid";
    https_enable = 0;
    https_cert_file = "./certs/server.crt";
    https_key_file = "./certs/server.key";
    auth_token = "";

    db_host = "127.0.0.1";
    db_port = 3306;
    db_user = "root";
    db_password = "";
    db_name = "qgydb";

    conn_timeout = 15;
    threadpool_max_threads = 16;
    threadpool_idle_timeout = 30;
    mysql_idle_timeout = 60;
    m_config_file_path = "server.conf";
}

void Config::load_default_file()
{
    load_file(m_config_file_path.c_str());
}

void Config::load_file(const char *path)
{
    if (path == NULL)
    {
        return;
    }

    ifstream file(path);
    if (!file.is_open())
    {
        return;
    }
    m_config_file_path = path;

    string line;
    while (getline(file, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        size_t pos = line.find('=');
        if (pos == string::npos)
        {
            continue;
        }

        string key = trim(line.substr(0, pos));
        string value = trim(line.substr(pos + 1));

        if (key == "port") PORT = atoi(value.c_str());
        else if (key == "log_write") LOGWrite = atoi(value.c_str());
        else if (key == "log_level") log_level = atoi(value.c_str());
        else if (key == "log_split_lines") log_split_lines = atoi(value.c_str());
        else if (key == "log_queue_size") log_queue_size = atoi(value.c_str());
        else if (key == "trig_mode") TRIGMode = atoi(value.c_str());
        else if (key == "opt_linger") OPT_LINGER = atoi(value.c_str());
        else if (key == "sql_num") sql_num = atoi(value.c_str());
        else if (key == "thread_num") thread_num = atoi(value.c_str());
        else if (key == "close_log") close_log = atoi(value.c_str());
        else if (key == "actor_model") actor_model = atoi(value.c_str());
        else if (key == "daemon_mode") daemon_mode = atoi(value.c_str());
        else if (key == "pid_file") pid_file = value;
        else if (key == "https_enable") https_enable = atoi(value.c_str());
        else if (key == "https_cert_file") https_cert_file = value;
        else if (key == "https_key_file") https_key_file = value;
        else if (key == "auth_token") auth_token = value;
        else if (key == "db_host") db_host = value;
        else if (key == "db_port") db_port = atoi(value.c_str());
        else if (key == "db_user") db_user = value;
        else if (key == "db_password") db_password = value;
        else if (key == "db_name") db_name = value;
        else if (key == "conn_timeout") conn_timeout = atoi(value.c_str());
        else if (key == "threadpool_max_threads") threadpool_max_threads = atoi(value.c_str());
        else if (key == "threadpool_idle_timeout") threadpool_idle_timeout = atoi(value.c_str());
        else if (key == "mysql_idle_timeout") mysql_idle_timeout = atoi(value.c_str());
    }
}

void Config::apply_env_overrides()
{
    PORT = getenv_int_value("TWS_PORT", PORT);
    LOGWrite = getenv_int_value("TWS_LOG_WRITE", LOGWrite);
    log_level = getenv_int_value("TWS_LOG_LEVEL", log_level);
    log_split_lines = getenv_int_value("TWS_LOG_SPLIT_LINES", log_split_lines);
    log_queue_size = getenv_int_value("TWS_LOG_QUEUE_SIZE", log_queue_size);
    TRIGMode = getenv_int_value("TWS_TRIG_MODE", TRIGMode);
    OPT_LINGER = getenv_int_value("TWS_OPT_LINGER", OPT_LINGER);
    sql_num = getenv_int_value("TWS_SQL_NUM", sql_num);
    thread_num = getenv_int_value("TWS_THREAD_NUM", thread_num);
    close_log = getenv_int_value("TWS_CLOSE_LOG", close_log);
    actor_model = getenv_int_value("TWS_ACTOR_MODEL", actor_model);
    daemon_mode = getenv_int_value("TWS_DAEMON_MODE", daemon_mode);
    https_enable = getenv_int_value("TWS_HTTPS_ENABLE", https_enable);
    db_port = getenv_int_value("TWS_DB_PORT", db_port);
    conn_timeout = getenv_int_value("TWS_CONN_TIMEOUT", conn_timeout);
    threadpool_max_threads = getenv_int_value("TWS_THREADPOOL_MAX_THREADS", threadpool_max_threads);
    threadpool_idle_timeout = getenv_int_value("TWS_THREADPOOL_IDLE_TIMEOUT", threadpool_idle_timeout);
    mysql_idle_timeout = getenv_int_value("TWS_MYSQL_IDLE_TIMEOUT", mysql_idle_timeout);

    const char *value = NULL;
    value = getenv_value("TWS_PID_FILE");
    if (value) pid_file = value;
    value = getenv_value("TWS_HTTPS_CERT_FILE");
    if (value) https_cert_file = value;
    value = getenv_value("TWS_HTTPS_KEY_FILE");
    if (value) https_key_file = value;
    value = getenv_value("TWS_AUTH_TOKEN");
    if (value) auth_token = value;
    value = getenv_value("TWS_DB_HOST");
    if (value) db_host = value;
    value = getenv_value("TWS_DB_USER");
    if (value) db_user = value;
    value = getenv_value("TWS_DB_PASSWORD");
    if (value) db_password = value;
    value = getenv_value("TWS_DB_NAME");
    if (value) db_name = value;
}

void Config::parse_arg(int argc, char*argv[]){
    int opt;
    const char *str = "f:p:l:m:o:s:t:c:a:d:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'f':
        {
            load_file(optarg);
            break;
        }
        case 'p':
        {
            PORT = atoi(optarg);
            break;
        }
        case 'l':
        {
            LOGWrite = atoi(optarg);
            break;
        }
        case 'm':
        {
            TRIGMode = atoi(optarg);
            break;
        }
        case 'o':
        {
            OPT_LINGER = atoi(optarg);
            break;
        }
        case 's':
        {
            sql_num = atoi(optarg);
            break;
        }
        case 't':
        {
            thread_num = atoi(optarg);
            break;
        }
        case 'c':
        {
            close_log = atoi(optarg);
            break;
        }
        case 'a':
        {
            actor_model = atoi(optarg);
            break;
        }
        case 'd':
        {
            daemon_mode = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
}

const string &Config::config_file_path() const
{
    return m_config_file_path;
}
