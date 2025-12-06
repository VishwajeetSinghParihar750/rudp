#pragma once
#include "udp.hpp"
#include "channel_manager.hpp"
#include "i_server.hpp"

#include <memory>

inline std::shared_ptr<i_client> create_connection(const char *host, const char *port)
{
    //
    std::shared_ptr<udp> udp_ = std::make_shared<udp>(host, port);

    std::shared_ptr<channel_manager> channel_manager_ = std::make_shared<channel_manager>();

    channel_manager_->set_udp_transport(udp_);
    udp_->set_channel_manager(channel_manager_);

    //
    return channel_manager_;
}
