#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <cstdio>
#include <atomic>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <pthread.h>
#include <queue>
#include <string>
#include <vector>
#include <time.h>
#include <errno.h>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    enum queue_mode
    {
        queue_mode_mutex = 0,
        queue_mode_lockfree = 1
    };

    /*thread_number是线程池初始线程数，max_requests是请求队列最大长度*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000,
               int max_thread_number = 0, int idle_timeout = 30,
               queue_mode mode = queue_mode_mutex);
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

    class bounded_mpmc_queue
    {
    public:
        explicit bounded_mpmc_queue(size_t capacity)
            : m_capacity(normalize_capacity(capacity)),
              m_mask(m_capacity - 1),
              m_buffer(new cell[m_capacity]),
              m_enqueue_pos(0),
              m_dequeue_pos(0)
        {
            for (size_t i = 0; i < m_capacity; ++i)
            {
                m_buffer[i].sequence.store(i, std::memory_order_relaxed);
            }
        }

        bool enqueue(const task &value)
        {
            size_t pos = m_enqueue_pos.load(std::memory_order_relaxed);
            while (true)
            {
                cell &current = m_buffer[pos & m_mask];
                size_t seq = current.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

                if (diff == 0)
                {
                    if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        current.payload = value;
                        current.sequence.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                }
                else if (diff < 0)
                {
                    return false;
                }
                else
                {
                    pos = m_enqueue_pos.load(std::memory_order_relaxed);
                }
            }
        }

        bool dequeue(task &value)
        {
            size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);
            while (true)
            {
                cell &current = m_buffer[pos & m_mask];
                size_t seq = current.sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

                if (diff == 0)
                {
                    if (m_dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        value = current.payload;
                        current.sequence.store(pos + m_mask + 1, std::memory_order_release);
                        return true;
                    }
                }
                else if (diff < 0)
                {
                    return false;
                }
                else
                {
                    pos = m_dequeue_pos.load(std::memory_order_relaxed);
                }
            }
        }

    private:
        struct cell
        {
            std::atomic<size_t> sequence;
            task payload;
        };

        static size_t normalize_capacity(size_t capacity)
        {
            size_t normalized = 1;
            while (normalized < capacity)
            {
                normalized <<= 1;
            }
            return normalized;
        }

        const size_t m_capacity;
        const size_t m_mask;
        std::unique_ptr<cell[]> m_buffer;
        std::atomic<size_t> m_enqueue_pos;
        std::atomic<size_t> m_dequeue_pos;
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
    std::atomic<int> m_current_thread_number;
    std::atomic<int> m_busy_thread_number;
    std::atomic<int> m_pending_task_number;
    int m_idle_timeout;
    int m_max_requests;
    std::atomic<bool> m_stop;
    queue_mode m_queue_mode;
    std::vector<pthread_t> m_threads;
    std::queue<task> m_mutex_workqueue;
    locker m_queuelocker;
    cond m_queuecond;
    bounded_mpmc_queue m_workqueue;
    sem m_queued_tasks;
    locker m_threadlocker;
    connection_pool *m_connPool;
    int m_actor_model;
};

template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests,
                          int max_thread_number, int idle_timeout, queue_mode mode)
    : m_min_thread_number(thread_number),
      m_max_thread_number(max_thread_number > 0 ? max_thread_number : thread_number * 2),
      m_current_thread_number(0),
      m_busy_thread_number(0),
      m_pending_task_number(0),
      m_idle_timeout(idle_timeout > 0 ? idle_timeout : 30),
      m_max_requests(max_requests),
      m_stop(false),
      m_queue_mode(mode),
      m_workqueue(static_cast<size_t>(max_requests)),
      m_queued_tasks(0),
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

    for (int i = 0; i < m_min_thread_number; ++i)
    {
        if (!spawn_worker_unlocked())
        {
            m_stop.store(true, std::memory_order_release);
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    m_stop.store(true, std::memory_order_release);

    if (m_queue_mode == queue_mode_lockfree)
    {
        int worker_count = m_current_thread_number.load(std::memory_order_acquire);
        for (int i = 0; i < worker_count; ++i)
        {
            m_queued_tasks.post();
        }
    }
    else
    {
        m_queuelocker.lock();
        m_queuecond.broadcast();
        m_queuelocker.unlock();
    }

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
    if (m_stop.load(std::memory_order_acquire))
    {
        return false;
    }

    task current_task;
    current_task.request = request;
    current_task.state = state;
    current_task.use_state = use_state;

    if (m_queue_mode == queue_mode_lockfree)
    {
        if (!m_workqueue.enqueue(current_task))
        {
            return false;
        }

        m_pending_task_number.fetch_add(1, std::memory_order_release);
        maybe_grow_unlocked();
        m_queued_tasks.post();
        return true;
    }

    m_queuelocker.lock();
    if (m_stop.load(std::memory_order_acquire) || (int)m_mutex_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_mutex_workqueue.push(current_task);
    maybe_grow_unlocked();
    m_queuecond.signal();
    m_queuelocker.unlock();
    return true;
}

template <typename T>
bool threadpool<T>::spawn_worker_unlocked()
{
    m_threadlocker.lock();
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, this) != 0)
    {
        m_threadlocker.unlock();
        return false;
    }
    m_threads.push_back(tid);
    m_current_thread_number.fetch_add(1, std::memory_order_release);
    m_threadlocker.unlock();
    return true;
}

template <typename T>
void threadpool<T>::maybe_grow_unlocked()
{
    if (m_queue_mode == queue_mode_mutex)
    {
        int pending_tasks = (int)m_mutex_workqueue.size();
        int idle_threads = m_current_thread_number.load(std::memory_order_acquire) - m_busy_thread_number.load(std::memory_order_acquire);

        while (pending_tasks > idle_threads && m_current_thread_number.load(std::memory_order_acquire) < m_max_thread_number)
        {
            if (!spawn_worker_unlocked())
            {
                break;
            }
            idle_threads = m_current_thread_number.load(std::memory_order_acquire) - m_busy_thread_number.load(std::memory_order_acquire);
        }
        return;
    }

    if (m_stop.load(std::memory_order_acquire))
    {
        return;
    }

    int pending_tasks = m_pending_task_number.load(std::memory_order_acquire);
    int idle_threads = m_current_thread_number.load(std::memory_order_acquire) - m_busy_thread_number.load(std::memory_order_acquire);

    while (pending_tasks > idle_threads && m_current_thread_number.load(std::memory_order_acquire) < m_max_thread_number)
    {
        if (!spawn_worker_unlocked())
        {
            break;
        }
        idle_threads = m_current_thread_number.load(std::memory_order_acquire) - m_busy_thread_number.load(std::memory_order_acquire);
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
    if (m_queue_mode == queue_mode_mutex)
    {
        while (true)
        {
            m_queuelocker.lock();
            while (m_mutex_workqueue.empty() && !m_stop.load(std::memory_order_acquire))
            {
                if (m_current_thread_number.load(std::memory_order_acquire) > m_min_thread_number)
                {
                    timespec timeout = make_timeout();
                    bool notified = m_queuecond.timewait(m_queuelocker.get(), timeout);
                    if (!notified && errno == ETIMEDOUT &&
                        m_mutex_workqueue.empty() &&
                        m_current_thread_number.load(std::memory_order_acquire) > m_min_thread_number)
                    {
                        m_current_thread_number.fetch_sub(1, std::memory_order_acq_rel);
                        m_queuelocker.unlock();
                        return;
                    }
                }
                else
                {
                    m_queuecond.wait(m_queuelocker.get());
                }
            }

            if (m_stop.load(std::memory_order_acquire) && m_mutex_workqueue.empty())
            {
                m_current_thread_number.fetch_sub(1, std::memory_order_acq_rel);
                m_queuelocker.unlock();
                return;
            }

            task current_task = m_mutex_workqueue.front();
            m_mutex_workqueue.pop();
            m_busy_thread_number.fetch_add(1, std::memory_order_acq_rel);
            m_queuelocker.unlock();

            execute(current_task);

            m_busy_thread_number.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    while (true)
    {
        bool acquired_task = false;
        task current_task;

        while (!acquired_task)
        {
            if (m_stop.load(std::memory_order_acquire) &&
                m_pending_task_number.load(std::memory_order_acquire) == 0)
            {
                m_current_thread_number.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }

            if (m_current_thread_number.load(std::memory_order_acquire) > m_min_thread_number)
            {
                timespec timeout = make_timeout();
                if (!m_queued_tasks.timewait(timeout))
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (errno == ETIMEDOUT &&
                        m_pending_task_number.load(std::memory_order_acquire) == 0 &&
                        m_current_thread_number.load(std::memory_order_acquire) > m_min_thread_number)
                    {
                        m_current_thread_number.fetch_sub(1, std::memory_order_acq_rel);
                        return;
                    }
                    continue;
                }
            }
            else
            {
                if (!m_queued_tasks.wait())
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    continue;
                }
            }

            if (m_workqueue.dequeue(current_task))
            {
                m_pending_task_number.fetch_sub(1, std::memory_order_acq_rel);
                acquired_task = true;
                break;
            }

            if (m_stop.load(std::memory_order_acquire))
            {
                m_current_thread_number.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
        }

        m_busy_thread_number.fetch_add(1, std::memory_order_acq_rel);

        execute(current_task);

        m_busy_thread_number.fetch_sub(1, std::memory_order_acq_rel);
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
