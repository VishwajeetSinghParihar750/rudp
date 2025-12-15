#pragma once

#include <sys/socket.h>
#include <memory>

class rudp_protocol_packet;
class transport_addr;

class i_session_control_for_udp
{
public:
    virtual ~i_session_control_for_udp() = default;

    virtual void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) = 0;
    virtual ssize_t send_packet_to_network(const char *buf, const size_t &len) = 0;
};