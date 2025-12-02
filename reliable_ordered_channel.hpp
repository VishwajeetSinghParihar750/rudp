#pragma once

#include "types.hpp"
#include <memory>
#include "i_packet.hpp"
#include "raw_packet.hpp"
#include "rudp_protocol.hpp"
#include "channel.hpp"

class reliable_ordered_channel : public channel
{

    // return how much u wrote
    // this might be kept not here btw
    size_t serialize_header_and_content(char *buf, size_t len)
    {
    }

public:
    reliable_ordered_channel(channel_id id, channel_type type) : channel(id, type)
    {
    }

    // recieve from client what came on udp
    void on_transport_receive(std::unique_ptr<i_packet> pkt) override
    {
    }

    //
    std::unique_ptr<i_packet> on_transport_send() override
    {
        std::unique_ptr<raw_packet> pkt = std::make_unique<raw_packet>(1500); // ℹ️ can be optimized

        size_t offset = rudp_protocol::CHANNEL_HEADER_OFFSET;
        serialize_header_and_content(pkt->get_buffer() + offset, pkt->get_capacity() - offset);

        return pkt;
    }

    // raed into application buffer what came from network
    ssize_t read_bytes_to_application(char *buf, const size_t &len) override {}

    // coming from application to send to network
    ssize_t write_bytes_from_application(const char *buf, const size_t &len) override
    {
    }
};