#pragma once

#include "types.hpp"
#include <memory>
#include "i_packet.hpp"
#include "raw_packet.hpp"
#include "rudp_protocol.hpp"

class channel
{
protected:
    channel_id channel_id_;
    client_id client_id_;

    channel_type channel_type_;

public:
    channel(channel_id id, channel_type type) : channel_id_(id), client_id_(invalid_client_id), channel_type_(type)
    {
    }

    void set_client_id(client_id cl_id)
    {
        client_id_ = cl_id;
    }

    //
    // recieve from client what came on udp
    virtual void on_transport_receive(const char *ibuf, const uint32_t &sz) = 0;

    virtual std::unique_ptr<i_packet> on_transport_send() = 0;

    // raed into application buffer what came from network
    virtual ssize_t read_bytes_to_application(char *buf, const size_t &len) = 0;

    // coming from application to send to network
    virtual ssize_t write_bytes_from_application(const char *buf, const size_t &len) = 0;
};