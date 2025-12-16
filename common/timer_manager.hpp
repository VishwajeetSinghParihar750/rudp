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