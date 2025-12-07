#pragma once

#include <chrono>
#include <iostream>
#include <functional>

using clock = std::chrono::steady_clock;
using timepoint = clock::time_point;
using duration_ms = std::chrono::milliseconds;

class timer_info
{
public:
    using callback = std::function<void()>;
    using timer_id = uint64_t;

private:
    timer_id id;
    timepoint expiration_time;
    callback on_expire;

public:
    timer_info(timer_id id_, duration_ms timeout_in_ms, callback on_expire_)
        : id(id_), expiration_time(clock::now() + timeout_in_ms), on_expire(on_expire_) {}

    bool has_expired() const
    {
        return clock::now() >= expiration_time;
    }

    void execute_on_expire_callback()
    {
        on_expire();
    }

    duration_ms time_remaining_in_ms() const
    {
        timepoint now = clock::now();
        if (now >= expiration_time)
        {
            return duration_ms(0);
        }
        return std::chrono::duration_cast<duration_ms>(expiration_time - now);
    }

    bool operator<(const timer_info &other) const
    {
        return expiration_time < other.expiration_time;
    }
};
