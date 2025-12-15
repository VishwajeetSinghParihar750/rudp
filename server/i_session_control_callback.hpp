#pragma once
#include <memory>
#include "types.hpp"

class rudp_protocol_packet;

class i_session_control_callback
{
public:
    virtual ~i_session_control_callback() = default;

    virtual void add_client(const client_id &cl_id) = 0;
    virtual void remove_client(const client_id &cl_id) = 0;
    virtual void on_transport_receive(const client_id &cl_id, std::unique_ptr<rudp_protocol_packet> pkt) = 0;
};