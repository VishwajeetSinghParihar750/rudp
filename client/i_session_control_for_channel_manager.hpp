#pragma once
#include <memory>
#include "../common/types.hpp"

class rudp_protocol_packet;

class i_session_control_for_channel_manager
{

public:
    virtual ~i_session_control_for_channel_manager() = default;

    virtual void on_transport_send_data(std::shared_ptr<rudp_protocol_packet> pkt) = 0;
    virtual void on_close_client() = 0;
};