#pragma once

#include <thread>
#include <memory>

#include "../common/thread_safe_priority_queue.hpp"
#include "timer_info.hpp"

using timer_info_ptr = std::shared_ptr<timer_info>;
struct timer_info_ptr_compare
{
    bool operator()(const timer_info_ptr &a, const timer_info_ptr &b) const
    {
        return *b < *a;
    }
};
class timer_manager
{
    std::jthread timer_manager_loop;

    thread_safe_priority_queue<timer_info_ptr, std::vector<timer_info_ptr>, timer_info_ptr_compare> timer_manager_q;

public:
    timer_manager() : timer_manager_loop([this](std::stop_token stoken)
                                         {
                            while (!stoken.stop_requested())  {

                                timer_info_ptr timerptr;
                                bool got_timer = timer_manager_q.wait_for_and_pop(timerptr, duration_ms(100));

                                if(stoken.stop_requested()) return;

                                if(got_timer && timerptr && timerptr->has_expired() ) {
                                    timerptr->execute_on_expire_callback();
                                }
                                else if(got_timer && timerptr) {
                                    std::this_thread::sleep_for(std::min(duration_ms(2), timerptr->time_remaining_in_ms())) ;
                                    timer_manager_q.push(std::move(timerptr)) ;
                                }
                           } }) {}

    void add_timer(std::unique_ptr<timer_info> timer_ptr)
    {
        timer_manager_q.push(std::make_shared<timer_info>(*timer_ptr));
    }
};

inline void add_timer_with_retries_exponential(std::weak_ptr<timer_manager> timer_mgr_weak,
                                               duration_ms initial_timeout,
                                               std::function<void()> cb,
                                               uint32_t max_retries,
                                               float backoff_factor = 2.0f)
{
    auto timer_mgr = timer_mgr_weak.lock();
    if (!timer_mgr || max_retries == 0)
        return;

    for (uint32_t i = 0; i < max_retries; i++)
    {
        // Calculate delay for this retry (exponential backoff)
        auto delay = duration_ms(static_cast<int64_t>(
            initial_timeout.count() * std::pow(backoff_factor, i)));

        std::function<void()> retry_cb = [cb, retry_num = i + 1]()
        {
            logger::getInstance().logTest("Executing retry " +
                                          std::to_string(retry_num));
            cb();
        };

        timer_mgr->add_timer(std::make_unique<timer_info>(delay, retry_cb));

        logger::getInstance().logTest("Scheduled retry " +
                                      std::to_string(i + 1) + "/" + std::to_string(max_retries) +
                                      " at " + std::to_string(delay.count()) + "ms");
    }
}