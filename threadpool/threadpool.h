#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <cstdio>
#include <exception>
#include <pthread.h>
#include <queue>
#include <vector>
#include <time.h>
#include <errno.h>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池初始线程数，max_requests是请求队列最大长度*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000,
               int max_thread_number = 0, int idle_timeout = 30);
    ~threadpool();

    bool append(T *request, int state);
    bool append_p(T *request);

private:
    struct task
    {
        T *request;
        int state;
        bool use_state;
    };

    static void *worker(void *arg);
    void run();
    bool enqueue(T *request, int state, bool use_state);
    bool spawn_worker_unlocked();
    void maybe_grow_unlocked();
    void execute(task &current_task);
    timespec make_timeout() const;

private:
    int m_min_thread_number;
    int m_max_thread_number;
    int m_current_thread_number;
    int m_busy_thread_number;
    int m_idle_timeout;
    int m_max_requests;
    bool m_stop;
    std::vector<pthread_t> m_threads;
    std::queue<task> m_workqueue;
    locker m_queuelocker;
    cond m_queuecond;
    connection_pool *m_connPool;
    int m_actor_model;
};

template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests,
                          int max_thread_number, int idle_timeout)
    : m_min_thread_number(thread_number),
      m_max_thread_number(max_thread_number > 0 ? max_thread_number : thread_number * 2),
      m_current_thread_number(0),
      m_busy_thread_number(0),
      m_idle_timeout(idle_timeout > 0 ? idle_timeout : 30),
      m_max_requests(max_requests),
      m_stop(false),
      m_connPool(connPool),
      m_actor_model(actor_model)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    if (m_max_thread_number < m_min_thread_number)
    {
        m_max_thread_number = m_min_thread_number;
    }

    m_threads.reserve(m_max_thread_number);

    m_queuelocker.lock();
    for (int i = 0; i < m_min_thread_number; ++i)
    {
        if (!spawn_worker_unlocked())
        {
            m_stop = true;
            m_queuecond.broadcast();
            m_queuelocker.unlock();
            throw std::exception();
        }
    }
    m_queuelocker.unlock();
}

template <typename T>
threadpool<T>::~threadpool()
{
    m_queuelocker.lock();
    m_stop = true;
    m_queuecond.broadcast();
    m_queuelocker.unlock();

    for (size_t i = 0; i < m_threads.size(); ++i)
    {
        pthread_join(m_threads[i], NULL);
    }
}

template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    return enqueue(request, state, true);
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    return enqueue(request, 0, false);
}

template <typename T>
bool threadpool<T>::enqueue(T *request, int state, bool use_state)
{
    m_queuelocker.lock();
    if (m_stop || (int)m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    task current_task;
    current_task.request = request;
    current_task.state = state;
    current_task.use_state = use_state;

    m_workqueue.push(current_task);
    maybe_grow_unlocked();
    m_queuecond.signal();
    m_queuelocker.unlock();
    return true;
}

template <typename T>
bool threadpool<T>::spawn_worker_unlocked()
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, this) != 0)
    {
        return false;
    }
    m_threads.push_back(tid);
    ++m_current_thread_number;
    return true;
}

template <typename T>
void threadpool<T>::maybe_grow_unlocked()
{
    int pending_tasks = (int)m_workqueue.size();
    int idle_threads = m_current_thread_number - m_busy_thread_number;

    while (pending_tasks > idle_threads && m_current_thread_number < m_max_thread_number)
    {
        if (!spawn_worker_unlocked())
        {
            break;
        }
        idle_threads = m_current_thread_number - m_busy_thread_number;
    }
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
timespec threadpool<T>::make_timeout() const
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += m_idle_timeout;
    return ts;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuelocker.lock();
        while (m_workqueue.empty() && !m_stop)
        {
            if (m_current_thread_number > m_min_thread_number)
            {
                timespec timeout = make_timeout();
                bool notified = m_queuecond.timewait(m_queuelocker.get(), timeout);
                if (!notified && errno == ETIMEDOUT && m_workqueue.empty() && m_current_thread_number > m_min_thread_number)
                {
                    --m_current_thread_number;
                    m_queuelocker.unlock();
                    return;
                }
            }
            else
            {
                m_queuecond.wait(m_queuelocker.get());
            }
        }

        if (m_stop && m_workqueue.empty())
        {
            --m_current_thread_number;
            m_queuelocker.unlock();
            return;
        }

        task current_task = m_workqueue.front();
        m_workqueue.pop();
        ++m_busy_thread_number;
        m_queuelocker.unlock();

        execute(current_task);

        m_queuelocker.lock();
        --m_busy_thread_number;
        m_queuelocker.unlock();
    }
}

template <typename T>
void threadpool<T>::execute(task &current_task)
{
    T *request = current_task.request;
    if (!request)
    {
        return;
    }

    request->lock_request();

    if (current_task.use_state)
    {
        request->m_state = current_task.state;
    }

    if (1 == m_actor_model)
    {
        if (0 == request->m_state)
        {
            if (request->read_once())
            {
                request->improv = 1;
                connectionRAII mysqlcon(&request->mysql, m_connPool);
                request->process();
            }
            else
            {
                request->improv = 1;
                request->timer_flag = 1;
            }
        }
        else
        {
            if (request->write())
            {
                request->improv = 1;
            }
            else
            {
                request->improv = 1;
                request->timer_flag = 1;
            }
        }
    }
    else
    {
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        request->process();
    }

    request->unlock_request();
}

#endif
