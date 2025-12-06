#pragma once

#include "types.hpp"

class i_sockaddr;
class i_packet;

class i_session_control_callback
{
public:
    virtual ~i_session_control_callback() = default;

    virtual void add_client(const client_id &cl_id) = 0;
    virtual void remove_client(const client_id &cl_id) = 0;
    virtual void send_control_packet_via_transport(const i_sockaddr &dest_addr, const client_id &cl_id, std::unique_ptr<i_packet> pkt) = 0;
};