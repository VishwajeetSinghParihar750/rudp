#pragma once
#include <memory>
#include "types.hpp"

class rudp_protocol_packet;

class i_session_control_callback
{
public:
    virtual ~i_session_control_callback() = default;

    virtual void on_server_disconnected() = 0;
    virtual void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) = 0;
};