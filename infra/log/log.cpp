#include "log.h"

#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std;

namespace
{
string build_dated_log_name(const tm &time_info, const string &prefix, const string &base_name, int file_index)
{
    char date_prefix[64] = {0};
    snprintf(date_prefix, sizeof(date_prefix), "%d_%02d_%02d_",
             time_info.tm_year + 1900, time_info.tm_mon + 1, time_info.tm_mday);

    string path = prefix + date_prefix + base_name;
    if (file_index > 0)
    {
        char suffix[32] = {0};
        snprintf(suffix, sizeof(suffix), ".%d", file_index);
        path += suffix;
    }
    return path;
}
}

Log::Log()
    : dir_name(),
      log_name(),
      m_split_lines(0),
      m_log_buf_size(0),
      m_count(0),
      m_today(0),
      m_fp(nullptr),
      m_log_queue(),
      m_max_queue_size(0),
      m_pending_logs(0),
      m_stop_requested(false),
      m_is_async(false),
      m_write_thread(),
      m_close_log(0),
      m_log_level(INFO),
      m_today_year(0),
      m_today_mon(0),
      m_file_index(0)
{
}

Log::~Log()
{
    shutdown_async();
    lock_guard<mutex> lock(m_file_mutex);
    m_fp.reset();
}

void Log::reset_queue_locked()
{
    m_log_queue.clear();
    m_pending_logs = 0;
    m_stop_requested = false;
}

//异步需要设置队列长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size, int log_level)
{
    shutdown_async();

    m_close_log = close_log;
    m_log_level = log_level;
    m_log_buf_size = log_buf_size;
    m_split_lines = split_lines;

    if (file_name == NULL)
    {
        return false;
    }

    time_t t = time(NULL);
    struct tm my_tm;
    localtime_r(&t, &my_tm);

    const char *p = strrchr(file_name, '/');
    if (p == NULL)
    {
        log_name = file_name;
        dir_name.clear();
    }
    else
    {
        log_name = p + 1;
        dir_name.assign(file_name, static_cast<size_t>(p - file_name + 1));
    }

    {
        lock_guard<mutex> queue_lock(m_queue_mutex);
        reset_queue_locked();
        m_max_queue_size = max_queue_size >= 1 ? static_cast<size_t>(max_queue_size) : 0;
    }

    {
        lock_guard<mutex> file_lock(m_file_mutex);
        m_count = 0;
        m_today_year = my_tm.tm_year + 1900;
        m_today_mon = my_tm.tm_mon + 1;
        m_today = my_tm.tm_mday;
        m_file_index = 0;

        const string log_full_name = build_log_file_path(my_tm, m_file_index);
        m_fp.reset(fopen(log_full_name.c_str(), "a"));
        if (m_fp == nullptr)
        {
            return false;
        }
    }

    if (max_queue_size >= 1)
    {
        try
        {
            m_is_async = true;
            m_write_thread = thread(&Log::worker_loop, this);
        }
        catch (const system_error &)
        {
            m_is_async = false;
            lock_guard<mutex> file_lock(m_file_mutex);
            m_fp.reset();
            return false;
        }
    }
    else
    {
        m_is_async = false;
    }

    return true;
}

const char *Log::level_name(int level) const
{
    switch (level)
    {
    case DEBUG:
        return "[debug]:";
    case INFO:
        return "[info]:";
    case WARN:
        return "[warn]:";
    case ERROR:
        return "[error]:";
    default:
        return "[info]:";
    }
}

string Log::build_log_file_path(const tm &time_info, int file_index) const
{
    return build_dated_log_name(time_info, dir_name, log_name, file_index);
}

void Log::rotate_file(const tm &my_tm)
{
    bool date_changed = (m_today_year != my_tm.tm_year + 1900) ||
                        (m_today_mon != my_tm.tm_mon + 1) ||
                        (m_today != my_tm.tm_mday);

    if (date_changed)
    {
        m_today_year = my_tm.tm_year + 1900;
        m_today_mon = my_tm.tm_mon + 1;
        m_today = my_tm.tm_mday;
        m_file_index = 0;
        m_count = 0;
    }
    else
    {
        ++m_file_index;
    }

    tm current_time = my_tm;
    m_fp.reset(fopen(build_log_file_path(current_time, m_file_index).c_str(), "a"));
}

void Log::write_to_file_locked(const char *data, size_t len, const tm &time_info)
{
    if (data == NULL || len == 0 || m_fp == nullptr)
    {
        return;
    }

    m_count++;

    if ((m_today_year != time_info.tm_year + 1900) ||
        (m_today_mon != time_info.tm_mon + 1) ||
        (m_today != time_info.tm_mday) ||
        (m_split_lines > 0 && m_count > 0 && m_count % m_split_lines == 0))
    {
        rotate_file(time_info);
    }

    if (m_fp != nullptr)
    {
        fwrite(data, 1, len, m_fp.get());
    }
}

void Log::write_log(int level, const char *format, ...)
{
    if (m_close_log != 0 || level < m_log_level)
    {
        return;
    }

    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm my_tm;
    localtime_r(&t, &my_tm);
    const char *s = level_name(level);

    va_list valst;
    va_start(valst, format);

    const size_t buffer_size = static_cast<size_t>(m_log_buf_size > 0 ? m_log_buf_size + 2 : 64);
    thread_local vector<char> local_buffer;
    if (local_buffer.size() < buffer_size)
    {
        local_buffer.resize(buffer_size);
    }

    char *buffer = local_buffer.data();
    int prefix_len = snprintf(buffer, buffer_size, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                              my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                              my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    if (prefix_len < 0)
    {
        buffer[0] = '\0';
        prefix_len = 0;
    }
    if (static_cast<size_t>(prefix_len) < buffer_size)
    {
        vsnprintf(buffer + prefix_len, buffer_size - static_cast<size_t>(prefix_len), format != NULL ? format : "", valst);
    }
    va_end(valst);

    size_t log_len = strnlen(buffer, buffer_size - 1);
    if (log_len + 1 >= buffer_size)
    {
        log_len = buffer_size - 2;
    }
    buffer[log_len++] = '\n';
    buffer[log_len] = '\0';

    if (m_is_async)
    {
        unique_lock<mutex> queue_lock(m_queue_mutex);
        if (!m_stop_requested && m_max_queue_size > 0 && m_log_queue.size() < m_max_queue_size)
        {
            QueuedLog queued_log;
            queued_log.data.assign(buffer, log_len);
            queued_log.time_info = my_tm;
            m_log_queue.push_back(std::move(queued_log));
            ++m_pending_logs;
            queue_lock.unlock();
            m_queue_cv.notify_one();
            return;
        }
    }

    lock_guard<mutex> file_lock(m_file_mutex);
    write_to_file_locked(buffer, log_len, my_tm);
}

void Log::flush(void)
{
    if (m_is_async)
    {
        unique_lock<mutex> queue_lock(m_queue_mutex);
        m_drained_cv.wait(queue_lock, [this] {
            return m_pending_logs == 0;
        });
    }

    lock_guard<mutex> file_lock(m_file_mutex);
    if (m_fp != nullptr)
    {
        fflush(m_fp.get());
    }
}

void Log::worker_loop()
{
    const size_t max_batch_size = 64;
    vector<QueuedLog> batch;
    batch.reserve(max_batch_size);

    while (true)
    {
        batch.clear();
        {
            unique_lock<mutex> queue_lock(m_queue_mutex);
            m_queue_cv.wait(queue_lock, [this] {
                return m_stop_requested || !m_log_queue.empty();
            });

            if (m_log_queue.empty() && m_stop_requested)
            {
                break;
            }

            const size_t batch_size = min(max_batch_size, m_log_queue.size());
            for (size_t i = 0; i < batch_size; ++i)
            {
                batch.push_back(std::move(m_log_queue.front()));
                m_log_queue.pop_front();
            }
        }

        {
            lock_guard<mutex> file_lock(m_file_mutex);
            for (size_t i = 0; i < batch.size(); ++i)
            {
                write_to_file_locked(batch[i].data.data(), batch[i].data.size(), batch[i].time_info);
            }
        }

        {
            lock_guard<mutex> queue_lock(m_queue_mutex);
            m_pending_logs -= batch.size();
            if (m_pending_logs == 0)
            {
                m_drained_cv.notify_all();
            }
        }
    }

    lock_guard<mutex> queue_lock(m_queue_mutex);
    if (m_pending_logs == 0)
    {
        m_drained_cv.notify_all();
    }
}

void Log::shutdown_async()
{
    {
        lock_guard<mutex> queue_lock(m_queue_mutex);
        m_stop_requested = true;
    }
    m_queue_cv.notify_all();

    if (m_write_thread.joinable())
    {
        m_write_thread.join();
    }

    {
        lock_guard<mutex> queue_lock(m_queue_mutex);
        reset_queue_locked();
    }
    m_is_async = false;
}
