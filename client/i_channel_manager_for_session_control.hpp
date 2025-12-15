#pragma once
#include <memory>
#include "types.hpp"

class rudp_protocol_packet;

class i_channel_manager_for_session_control
{
public:
    virtual ~i_channel_manager_for_session_control() = default;

    virtual void on_server_disconnected() = 0;
    virtual void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) = 0;
};