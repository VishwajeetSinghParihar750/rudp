#pragma once

#include "types.hpp"
#include <memory>
#include "i_packet.hpp"
#include "raw_packet.hpp"
#include "rudp_protocol.hpp"
#include "timer_info.hpp"

using timer_info_ptr = std::shared_ptr<timer_info>;
using duration_ms = ::duration_ms;
using timer_callback = timer_info::callback;
using timer_allocator_t = std::function<timer_info_ptr(duration_ms, timer_callback)>;

// these should have info about one complete packet and never return half packet read when asked to be read
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

// payload should be always in network order
struct channel_setup_info
{
    channel_id ch_id;
    std::unique_ptr<i_packet> payload;
};


class i_channel
{

public:
    virtual std::unique_ptr<i_channel> clone() const = 0;

    // recieve from client what came on udp
    virtual void on_transport_receive(const char *ibuf, const uint32_t &sz) = 0;
    virtual channel_id get_channel_id() const = 0;

    virtual std::unique_ptr<i_packet> on_transport_send() = 0;

    // raed into application buffer what came from network
    virtual ssize_t read_bytes_to_application(char *buf, const uint32_t &len) = 0;

    // coming from application to send to network
    virtual ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) = 0;

    //
    virtual channel_setup_info get_channel_setup_info() = 0;
    virtual void process_channel_setup_info(channel_setup_info) = 0;

    //
    virtual void set_app_data_ready_notifier(std::function<void()>) = 0;
    virtual void set_net_data_ready_notifier(std::function<void()>) = 0;

    // Timer-related API
    virtual void set_timer_allocator(timer_allocator_t allocator) = 0;

};