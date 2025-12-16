#pragma once

#include "channel_manager.hpp"
#include "../common/timer_manager.hpp"
#include "session_control.hpp"
#include "udp.hpp"

#include <memory>

inline std::shared_ptr<i_server> create_server(const char *PORT)
{
    auto channel_manager_ = std::make_shared<channel_manager>();
    auto session_control_ = std::make_shared<session_control>();
    auto udp_ = std::make_shared<udp>(PORT);
    auto timer_manager_ = std::make_shared<timer_manager>();

    channel_manager_->set_session_control(session_control_, {});
    channel_manager_->set_timer_manager(timer_manager_, {});

    session_control_->set_channel_manager(channel_manager_, {});
    session_control_->set_timer_manager(timer_manager_, {});
    session_control_->set_udp(udp_, {});

    udp_->set_sesion_control(session_control_, {});

    return channel_manager_;
}
