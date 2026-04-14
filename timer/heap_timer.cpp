#include "heap_timer.h"

#include <algorithm>

void HeapTimer::add_or_update(int sockfd, int timeout)
{
    const time_t expire = time(nullptr) + timeout;
    std::unordered_map<int, size_t>::iterator it = m_index.find(sockfd);
    if (it == m_index.end())
    {
        m_heap.push_back(TimerNode{sockfd, expire});
        const size_t index = m_heap.size() - 1;
        m_index[sockfd] = index;
        sift_up(index);
        return;
    }

    const size_t index = it->second;
    m_heap[index].expire = expire;
    if (!sift_up(index))
    {
        sift_down(index);
    }
}

void HeapTimer::remove(int sockfd)
{
    std::unordered_map<int, size_t>::iterator it = m_index.find(sockfd);
    if (it == m_index.end())
    {
        return;
    }
    delete_at(it->second);
}

void HeapTimer::tick(const std::function<void(int)> &on_expire)
{
    if (!on_expire)
    {
        return;
    }

    const time_t now = time(nullptr);
    while (!m_heap.empty())
    {
        const TimerNode &node = m_heap.front();
        if (node.expire > now)
        {
            break;
        }

        const int sockfd = node.sockfd;
        delete_at(0);
        on_expire(sockfd);
    }
}

int HeapTimer::get_next_timeout_ms() const
{
    if (m_heap.empty())
    {
        return -1;
    }

    const time_t now = time(nullptr);
    if (m_heap.front().expire <= now)
    {
        return 0;
    }
    return static_cast<int>((m_heap.front().expire - now) * 1000);
}

bool HeapTimer::sift_up(size_t index)
{
    bool moved = false;
    while (index > 0)
    {
        const size_t parent = (index - 1) / 2;
        if (m_heap[parent].expire <= m_heap[index].expire)
        {
            break;
        }
        swap_node(parent, index);
        index = parent;
        moved = true;
    }
    return moved;
}

void HeapTimer::sift_down(size_t index)
{
    const size_t size = m_heap.size();
    while (true)
    {
        size_t smallest = index;
        const size_t left = index * 2 + 1;
        const size_t right = left + 1;

        if (left < size && m_heap[left].expire < m_heap[smallest].expire)
        {
            smallest = left;
        }
        if (right < size && m_heap[right].expire < m_heap[smallest].expire)
        {
            smallest = right;
        }
        if (smallest == index)
        {
            break;
        }

        swap_node(index, smallest);
        index = smallest;
    }
}

void HeapTimer::delete_at(size_t index)
{
    const size_t last = m_heap.size() - 1;
    const int sockfd = m_heap[index].sockfd;

    if (index != last)
    {
        swap_node(index, last);
    }

    m_index.erase(sockfd);
    m_heap.pop_back();

    if (index >= m_heap.size())
    {
        return;
    }

    if (!sift_up(index))
    {
        sift_down(index);
    }
}

void HeapTimer::swap_node(size_t lhs, size_t rhs)
{
    std::swap(m_heap[lhs], m_heap[rhs]);
    m_index[m_heap[lhs].sockfd] = lhs;
    m_index[m_heap[rhs].sockfd] = rhs;
}
