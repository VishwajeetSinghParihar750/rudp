#pragma once
#include <memory>

class timer_info;

class i_timer_service
{
public:
    virtual ~i_timer_service() = default;
    virtual void add_timer(std::shared_ptr<timer_info>) = 0;
};
