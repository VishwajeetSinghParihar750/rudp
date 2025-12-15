#pragma once
#include <memory>
#include "types.hpp"

class rudp_protocol_packet;

class i_channel_manager_callback
{

public:
    ~i_channel_manager_callback() = default;

    virtual void on_transport_send_data( std::unique_ptr<rudp_protocol_packet> pkt) = 0;
    virtual void on_notifying_server_close_to_application() = 0;
    virtual void on_close_client() = 0;
};