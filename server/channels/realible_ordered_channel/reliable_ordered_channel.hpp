
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

public:
    reliable_ordered_channel(channel_id ch_id_) : ch_id(ch_id_) {}

    void set_channel_id(channel_id ch_id_) { ch_id = ch_id_; }
    channel_id get_channel_id() { return ch_id; }
    std::unique_ptr<i_channel> clone() const { return std::make_unique<reliable_ordered_channel>(*this); }

    // payload in netowrk byte order
    channel_setup_info get_channel_setup_info()
    {
    }
    void process_channel_setup_info(channel_setup_info) {}

    rcv_block_info get_next_rcv_block_info() {} // this moved the to read block ahead
    send_block_info get_next_send_block_info() {}

    ssize_t read_rcv_block(rcv_block_info, char *buf, const uint32_t &len) {}
    std::unique_ptr<i_packet> read_send_block(send_block_info) {}

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
        auto pkt = std::make_unique<raw_packet>(1500);
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