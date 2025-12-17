#pragma once
#include <memory>
#include "../common/types.hpp"

class rudp_protocol_packet;

class i_session_control_for_channel_manager
{

public:
    ~i_session_control_for_channel_manager() = default;

    virtual void on_transport_send_data(const client_id &cl_id, std::unique_ptr<rudp_protocol_packet> pkt) = 0;
    virtual void notify_removal_of_client(const client_id &) = 0;
    virtual void on_close_server() = 0;
};