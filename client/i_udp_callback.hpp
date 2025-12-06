#pragma once

#include <sys/socket.h>
#include <memory>
#include "raw_packet.hpp"
#include "i_sockaddr.hpp"

class i_udp_callback
{
public:
    virtual ~i_udp_callback() = default;

    virtual ssize_t on_transport_receive(std::unique_ptr<i_packet> pkt, std::unique_ptr<i_sockaddr> source_addr) = 0;
};