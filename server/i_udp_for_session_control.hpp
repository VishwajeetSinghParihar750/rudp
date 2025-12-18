#pragma once
#include <unistd.h>

class transport_addr;

class i_udp_for_session_control
{
public:
    virtual ~i_udp_for_session_control() = default;

    virtual ssize_t send_packet_to_network(const transport_addr &addr, const char *buf, const size_t &len) = 0;
};
