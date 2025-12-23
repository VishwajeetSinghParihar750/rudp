#pragma once

#include <memory>
#include <functional>

#include "rudp_protocol_packet.hpp"

class i_timer_service;

class i_channel
{

public:
    virtual ~i_channel() = default;

    virtual std::unique_ptr<i_channel> clone() const = 0;

    // recieve from client what came on udp
    virtual void on_transport_receive(std::unique_ptr<rudp_protocol_packet>) = 0;

    // raed into application buffer what came from network
    virtual ssize_t read_bytes_to_application(char *buf, const uint32_t &len) = 0;

    // coming from application to send to network
    virtual ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) = 0;

    //
    virtual void set_on_app_data_ready(std::function<void()>) = 0;

    virtual void set_on_net_data_ready(std::function<void(std::shared_ptr<rudp_protocol_packet>)>) = 0;

    // Timer-related API
    virtual void set_timer_service(std::shared_ptr<i_timer_service>) = 0;
};