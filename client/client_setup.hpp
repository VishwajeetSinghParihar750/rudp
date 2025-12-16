#pragma once

#include <memory>
#include <string>

#include "i_client.hpp"

#include "channel_manager.hpp"
#include "../common/timer_manager.hpp"
#include "session_control.hpp"
#include "udp.hpp"

std::shared_ptr<i_client> create_client(const char *HOST, const char *PORT)
{

    auto channel_manager_ = std::make_shared<channel_manager>();
    auto session_control_ = std::make_shared<session_control>();
    auto udp_ = std::make_shared<udp>(HOST, PORT);
    auto timer_manager_ = std::make_shared<timer_manager>();

    channel_manager_->set_session_control(session_control_, {});
    channel_manager_->set_timer_manager(timer_manager_, {});

    session_control_->set_channel_manager(channel_manager_, {});
    session_control_->set_timer_manager(timer_manager_, {});
    session_control_->set_udp(udp_, {});

    udp_->set_sesion_control(session_control_, {});

    return channel_manager_;
}
