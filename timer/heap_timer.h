#ifndef HEAP_TIMER_H
#define HEAP_TIMER_H

#include <time.h>

#include <functional>
#include <unordered_map>
#include <vector>

class HeapTimer
{
public:
    struct TimerNode
    {
        int sockfd;
        time_t expire;
    };

public:
    void add_or_update(int sockfd, int timeout);
    void remove(int sockfd);
    void tick(const std::function<void(int)> &on_expire);
    int get_next_timeout_ms() const;

private:
    bool sift_up(size_t index);
    void sift_down(size_t index);
    void delete_at(size_t index);
    void swap_node(size_t lhs, size_t rhs);

private:
    std::vector<TimerNode> m_heap;
    std::unordered_map<int, size_t> m_index;
};

#endif
