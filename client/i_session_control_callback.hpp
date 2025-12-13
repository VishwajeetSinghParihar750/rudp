#pragma once

#include "types.hpp"

class i_sockaddr;
class i_packet;
class channel_setup_info;

class i_session_control_callback
{
public:
    virtual ~i_session_control_callback() = default;

    virtual void add_client(const client_id &cl_id, const i_sockaddr &sock_addr) = 0;
    virtual void remove_client(const client_id &cl_id) = 0;
    virtual void send_control_packet_via_transport(const client_id &cl_id, std::unique_ptr<i_packet> pkt) = 0;

    virtual void add_channel_for_client(const client_id &, const channel_id &) = 0;
    virtual void remove_channel_for_client(const client_id &, const channel_id &) = 0;
    virtual void process_channel_setup_request(const client_id &cl_id, channel_setup_info) = 0;
    virtual channel_setup_info get_channel_setup_info(const client_id &cl_id, const channel_id &) = 0;

    virtual ssize_t read_from_control_channel_nonblocking(channel_id &channel_id_, client_id &client_id_, char *buf, const uint32_t &len) = 0;
    virtual ssize_t read_from_control_channel_blocking(channel_id &channel_id_, client_id &client_id_, char *buf, const uint32_t &len) = 0;

    virtual ssize_t write_to_channel(const channel_id &channel_id_, const client_id &client_id_, const char *buf, const uint32_t &len) = 0;
};