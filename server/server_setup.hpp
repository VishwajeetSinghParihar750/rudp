#pragma once

#include "channel_manager.hpp"
#include "i_server.hpp"
#include "timer_manager.hpp"

#include <memory>

inline std::shared_ptr<i_server> create_server(const char *PORT = "4004")
{
    // Create channel manager (which manages timers and session control internally)
    auto channel_manager_ = std::make_shared<channel_manager>();

    return channel_manager_;
}
