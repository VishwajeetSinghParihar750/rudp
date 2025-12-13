#pragma once

#include <memory>
#include <functional>

#include "timer_info.hpp"
class rudp_protocol_packet;

using timer_info_ptr = std::shared_ptr<timer_info>;
using duration_ms = ::duration_ms;
using timer_callback = timer_info::callback;
using timer_allocator_t = std::function<timer_info_ptr(duration_ms, timer_callback)>;

class rudp_protocol_packet;

class i_channel
{

public:
    virtual std::unique_ptr<i_channel> clone() const = 0;

    // recieve from client what came on udp
    virtual void on_transport_receive(std::unique_ptr<rudp_protocol_packet>) = 0;

    virtual std::unique_ptr<rudp_protocol_packet> on_transport_send() = 0;

    // raed into application buffer what came from network
    virtual ssize_t read_bytes_to_application(char *buf, const uint32_t &len) = 0;

    // coming from application to send to network
    virtual ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) = 0;

    //
    virtual void set_on_app_data_ready(std::function<void()>) = 0;

    virtual void set_on_net_data_ready(std::function<void(std::unique_ptr<rudp_protocol_packet>)>) = 0;

    // Timer-related API
    virtual void set_timer_allocator(timer_allocator_t allocator) = 0;
};