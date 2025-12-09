#pragma once

#include "types.hpp"
#include <memory>
#include "i_packet.hpp"
#include "raw_packet.hpp"
#include "rudp_protocol.hpp"

struct rcv_block_info
{
    uint64_t block_identifier;
    uint64_t length;
};
struct send_block_info
{
    uint64_t block_identifier;
    uint64_t length;
};

struct channel_setup_info
{
    channel_id ch_id;
    std::unique_ptr<i_packet> payload;
};
class channel
{
protected:
    channel_id channel_id_;
    client_id client_id_;

public:
    channel(channel_id id) : channel_id_(id), client_id_(INVALID_CLIENT_ID)
    {
    }

    void set_client_id(client_id cl_id)
    {
        client_id_ = cl_id;
    }

    //

    virtual rcv_block_info get_next_rcv_block_info() = 0; // this moved the to read block ahead
    virtual send_block_info get_next_send_block_info() = 0;

    virtual ssize_t read_rcv_block(rcv_block_info, char *buf, const uint32_t &len) = 0;
    virtual std::unique_ptr<i_packet> read_send_block(send_block_info) = 0;

    // recieve from client what came on udp
    virtual void on_transport_receive(const char *ibuf, const uint32_t &sz) = 0;

    virtual std::unique_ptr<i_packet> on_transport_send() = 0;

    // raed into application buffer what came from network
    virtual ssize_t read_bytes_to_application(char *buf, const size_t &len) = 0;

    // coming from application to send to network
    virtual ssize_t write_bytes_from_application(const char *buf, const size_t &len) = 0;

    //
    virtual channel_setup_info get_channel_setup_info() = 0;
    virtual void process_channel_setup_info(channel_setup_info) = 0;
};