
#pragma once

#include "windows.hpp"
#include "../../i_channel.hpp"
#include "../../raw_packet.hpp"

class reliable_ordered_channel : public i_channel
{
    send_window snd_window;
    receive_window rcv_window;

    channel_id ch_id;

    std::function<void()> on_app_data_ready, on_net_data_ready;

    static constexpr uint16_t MAX_RELIABLE_ORDERED_CHANNEL_PACKET_SIZE = 1400;

public:
    reliable_ordered_channel(channel_id ch_id_) : ch_id(ch_id_) {}

    channel_id get_channel_id() const override { return ch_id; }
    std::unique_ptr<i_channel> clone() const override { return std::make_unique<reliable_ordered_channel>(*this); }

    // payload in netowrk byte order
    channel_setup_info get_channel_setup_info() override
    {
    }
    void process_channel_setup_info(channel_setup_info) override
    {
    }

    // Incoming Packet from Network
    void on_transport_receive(const char *ibuf, const uint32_t &sz) override
    {
        rudp_header h;
        uint32_t valid_sz = rcv_window.receive_packet(ibuf, sz, h);

        if (valid_sz > 0)
        {
            // 1. Update Sender with Remote Window size
            snd_window.set_remote_window(h.win_sz);
            // 2. Process the ACK carried in this packet
            snd_window.process_ack(h.ack_no);
        }
    }

    // Outgoing Packet to Network
    std::unique_ptr<i_packet> on_transport_send() override
    {
        uint16_t sz = MAX_RELIABLE_ORDERED_CHANNEL_PACKET_SIZE + rudp_protocol::CHANNEL_HEADER_OFFSET;

        auto pkt = std::make_unique<raw_packet>(sz);
        size_t offset = rudp_protocol::CHANNEL_HEADER_OFFSET;

        // CRITICAL FIX: Piggyback ACK and Window Size
        uint32_t ack = rcv_window.get_ack_no();
        uint16_t win = rcv_window.get_win_sz();

        uint32_t len = snd_window.send_packet(pkt->get_buffer() + offset, ack, win);

        if (len == 0)
            return nullptr;

        pkt->set_length(offset + len);
        return pkt;
    }

    ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
    {
        return rcv_window.read_from_buffer(buf, len);
    }

    ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
    {
        return snd_window.write_into_buffer(buf, len);
    }

    void set_app_data_ready_notifier(std::function<void()> f) override { on_app_data_ready = f; }
    void set_net_data_ready_notifier(std::function<void()> f) override { on_net_data_ready = f; }
};