#pragma once

#include <memory>
#include <vector>

#include "thread_safe_priority_queue.hpp"
#include "timer_info.hpp"

using timer_info_ptr = std::shared_ptr<timer_info>;

struct timer_info_ptr_compare
{
    bool operator()(const timer_info_ptr &a,
                    const timer_info_ptr &b) const
    {
        return *b < *a; // min-heap
    }
};

class timer_manager
{
    thread_safe_priority_queue<
        timer_info_ptr,
        std::vector<timer_info_ptr>,
        timer_info_ptr_compare>
        q;

public:
    // returns true if this timer became the earliest
    bool add_timer(timer_info_ptr t)
    {
        if (t == nullptr)
            return false;

        timer_info_ptr top;
        bool has_top = q.top(top);

        if (!has_top || !top)
        {
            q.push(std::move(t));
            return true;
        }

        bool toret = t->time_remaining_in_ms() < top->time_remaining_in_ms();
        q.push(std::move(t));

        return toret;
    }

    void process_expired()
    {
        timer_info_ptr t;

        while (true)
        {

            if (!q.pop(t) || !t)
                return;

            if (!t->has_expired())
            {
                q.push(std::move(t));
                return;
            }

            t->execute_on_expire_callback();
        }
    }

    // milliseconds until next timer, or large value
    duration_ms next_timeout(duration_ms max_wait)
    {
        timer_info_ptr t;
        if (!q.top(t) || !t)
            return max_wait;

        return std::min(max_wait, t->time_remaining_in_ms());
    }
};
