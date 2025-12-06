#pragma once

#include <chrono>
#include <iostream>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

struct timer_info
{
    TimePoint expiration_time;

    timer_info(Duration timeout)
        : expiration_time(Clock::now() + timeout) {}

    timer_info(TimePoint expiration)
        : expiration_time(expiration) {}

    timer_info() : expiration_time(TimePoint::max()) {}

    bool has_expired() const
    {
        return Clock::now() >= expiration_time;
    }

    Duration time_remaining() const
    {
        TimePoint now = Clock::now();
        if (now >= expiration_time)
        {
            return Duration(0);
        }
        return std::chrono::duration_cast<Duration>(expiration_time - now);
    }
};
