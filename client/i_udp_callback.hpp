#pragma once

#include <sys/socket.h>
#include <memory>

class rudp_protocol_packet;
class transport_addr;

class i_udp_callback
{
public:
    virtual ~i_udp_callback() = default;

    virtual void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt, std::unique_ptr<transport_addr> source_addr) = 0;
    virtual ssize_t send_packet_to_network(const transport_addr &addr, const char *buf, const size_t &len) = 0;
};