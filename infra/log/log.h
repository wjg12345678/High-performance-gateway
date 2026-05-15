#ifndef LOG_H
#define LOG_H

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <stdarg.h>
#include <string>
#include <thread>

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

    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0, int log_level = INFO);

    void write_log(int level, const char *format, ...);

    void flush(void);

private:
    struct FileCloser
    {
        void operator()(FILE *file) const
        {
            if (file != nullptr)
            {
                fflush(file);
                fclose(file);
            }
        }
    };

    struct QueuedLog
    {
        QueuedLog() : data(), time_info() {}

        std::string data;
        tm time_info;
    };

    using FileHandle = std::unique_ptr<FILE, FileCloser>;

    Log();
    virtual ~Log();
    void worker_loop();
    void shutdown_async();
    void write_to_file_locked(const char *data, size_t len, const tm &time_info);

private:
    const char *level_name(int level) const;
    void rotate_file(const tm &my_tm);
    std::string build_log_file_path(const tm &time_info, int file_index) const;
    void reset_queue_locked();

    std::string dir_name; //路径名
    std::string log_name; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FileHandle m_fp;    //打开log的文件指针
    std::deque<QueuedLog> m_log_queue;
    size_t m_max_queue_size;
    size_t m_pending_logs;
    bool m_stop_requested;
    bool m_is_async;                  //是否同步标志位
    std::thread m_write_thread;
    std::mutex m_file_mutex;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::condition_variable m_drained_cv;
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
