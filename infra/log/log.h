#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <time.h>
#include <atomic>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    enum LEVEL
    {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        ERROR = 3
    };

    //C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    static void *flush_log_thread(void *args)
    {
        (void)args;
        return Log::get_instance()->async_write_log();
    }
    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0, int log_level = INFO);

    void write_log(int level, const char *format, ...);

    void flush(void);

private:
    struct QueuedLog
    {
        QueuedLog() : data(), time_info() {}

        std::string data;
        tm time_info;
    };

    Log();
    virtual ~Log();
    void *async_write_log();
    void shutdown_async();
    void write_to_file_locked(const char *data, size_t len, const tm &time_info);

private:
    const char *level_name(int level) const;
    void rotate_file(const tm &my_tm);
    std::string build_log_file_path(const tm &time_info, int file_index) const;

    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FILE *m_fp;         //打开log的文件指针
    block_queue<QueuedLog> *m_log_queue; //阻塞队列
    std::atomic<int> m_pending_logs;
    bool m_is_async;                  //是否同步标志位
    bool m_thread_started;
    pthread_t m_write_thread;
    locker m_mutex;
    int m_close_log; //关闭日志
    int m_log_level; //最小日志级别
    int m_today_year;
    int m_today_mon;
    int m_file_index;
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__);}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__);}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__);}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__);}

#endif
