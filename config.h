#ifndef CONFIG_H
#define CONFIG_H

#include "webserver.h"

using namespace std;

class Config
{
public:
    Config();
    ~Config(){};

    void load_default_file();
    void load_file(const char *path);
    void apply_env_overrides();
    void parse_arg(int argc, char*argv[]);
    const string &config_file_path() const;

    //端口号
    int PORT;

    //日志写入方式
    int LOGWrite;

    //日志级别与滚动
    int log_level;
    int log_split_lines;
    int log_queue_size;

    //触发组合模式
    int TRIGMode;

    //listenfd触发模式
    int LISTENTrigmode;

    //connfd触发模式
    int CONNTrigmode;

    //优雅关闭链接
    int OPT_LINGER;

    //数据库连接池数量
    int sql_num;

    //线程池内的线程数量
    int thread_num;

    //是否关闭日志
    int close_log;

    //并发模型选择
    int actor_model;

    //守护进程模式
    int daemon_mode;

    //pid 文件
    string pid_file;

    //HTTPS 配置
    int https_enable;
    string https_cert_file;
    string https_key_file;
    string auth_token;

    //数据库配置
    string db_host;
    int db_port;
    string db_user;
    string db_password;
    string db_name;

    //超时与线程池配置
    int conn_timeout;
    int threadpool_max_threads;
    int threadpool_idle_timeout;
    int mysql_idle_timeout;
    string threadpool_queue_mode;

private:
    string m_config_file_path;
};

#endif
