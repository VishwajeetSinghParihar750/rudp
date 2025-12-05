#pragma once

#include <memory>
#include <assert.h>
#include <map>

#include "types.hpp"
#include "i_packet.hpp"
#include "raw_packet.hpp"
#include "rudp_protocol.hpp"
#include "channel.hpp"

//  ℹ️ resize buffer and all that can be added
class send_window
{
    struct header
    {
        uint32_t seq_no;
        uint32_t ack_no;
        uint16_t win_sz;
        uint16_t flags; // 00000... syn ack fin rst [ 0s are reserved ]
        uint16_t checksum;
    };
    // 14 bytes
    static constexpr uint16_t HEADER_SIZE = 14;
    static constexpr uint32_t DEFAULT_BUFFER_SIZE = 4096;
    static constexpr uint16_t MAX_MESSAGE_TRANSMISSION_SIZE = 1400;

    char *buf;
    uint32_t buf_size;
    uint16_t rcv_window_size;
    uint32_t sent_not_acked_start;
    uint32_t can_be_sent_start;
    uint32_t can_be_written_start;

    static constexpr uint32_t MAX_SEQUENCE_NUMBER = (1ll << 32) - 1;
    uint32_t cur_seq_no;
    uint32_t cur_ack_no;

    //
    uint32_t read_into_buffer(const char *ibuf, const uint32_t &sz)
    {
        uint32_t sent_not_acked_sz = (can_be_sent_start - sent_not_acked_start + buf_size) % buf_size;
        uint32_t can_be_sent_sz = (can_be_written_start - can_be_sent_start + buf_size) % buf_size;

        uint32_t can_read = buf_size - (sent_not_acked_sz + can_be_sent_sz);
        uint32_t to_read = std::min(can_read, sz);

        uint32_t to_read_ahead = std::min(to_read, buf_size - can_be_written_start);
        memcpy(buf + can_be_written_start, ibuf, to_read_ahead);

        uint32_t to_read_at_front = to_read - to_read_ahead;
        memcpy(buf, ibuf + to_read_ahead, to_read_at_front);

        can_be_written_start = (can_be_written_start + to_read) % buf_size;

        return to_read;
    }

    header get_header_for_current_packet()
    {
        //
        header h;
        h.seq_no = cur_seq_no;
        h.ack_no = can_be_written_start;
        h.win_sz = rcv_window_size;

        h.flags = 0; //  ⚠️to add

        h.checksum = 0;
    }

    uint16_t get_to_send_size()
    {

        uint32_t win_end = (sent_not_acked_start + rcv_window_size) % buf_size;

        uint32_t d1 = (win_end - can_be_sent_start) % buf_size, d2 = (can_be_sent_start - sent_not_acked_start) % buf_size;

        if (d1 + d2 != rcv_window_size) // means can be sent in not in the window
            return 0;

        uint32_t can_be_sent_sz = (can_be_written_start - can_be_sent_start + buf_size) % buf_size;
        uint32_t in_window_sz = (win_end - can_be_sent_start + buf_size) % buf_size;

        can_be_sent_sz = std::min(can_be_sent_sz, in_window_sz);

        uint32_t to_send = std::min(can_be_sent_sz, (uint32_t)MAX_MESSAGE_TRANSMISSION_SIZE);

        return to_send;
    }

    uint32_t fold_and_accumulate(uint32_t sum)
    {
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        return sum;
    }
    uint16_t serialize_header(char *buf, const uint32_t &sz)
    {
        header h = get_header_for_current_packet();

        uint32_t seq_no = htonl(h.seq_no);
        uint32_t ack_no = htonl(h.ack_no);
        uint16_t win_sz = htons(h.win_sz);
        uint16_t flags = htons(h.flags);

        uint16_t zero_csum = 0;

        memcpy(buf, &seq_no, sizeof(uint32_t));
        memcpy(buf + 4, &ack_no, sizeof(uint32_t));
        memcpy(buf + 8, &win_sz, sizeof(uint16_t));
        memcpy(buf + 10, &flags, sizeof(uint16_t));
        memcpy(buf + 12, &zero_csum, sizeof(uint16_t));

        uint32_t accumulated_sum = 0;

        for (uint32_t i = 0; i < 7; ++i)
        {
            uint32_t offset = i * 2;
            uint16_t high_byte = static_cast<uint8_t>(buf[offset]);
            uint16_t low_byte = static_cast<uint8_t>(buf[offset + 1]);

            accumulated_sum += (high_byte << 8) | low_byte;
        }

        return static_cast<uint16_t>(fold_and_accumulate(accumulated_sum));
    }

    uint16_t serialize_payload(char *buf, const uint32_t &sz)
    {
        uint16_t to_send_sz = get_to_send_size();
        uint32_t accumulated_sum = 0;

        uint32_t num_16bit_words = to_send_sz / 2;

        for (uint32_t i = 0; i < num_16bit_words; ++i)
        {
            uint32_t offset = i * 2;
            uint16_t high_byte = static_cast<uint8_t>(buf[offset]);
            uint16_t low_byte = static_cast<uint8_t>(buf[offset + 1]);

            accumulated_sum += (high_byte << 8) | low_byte;
        }

        if (to_send_sz & 1)
        {
            uint8_t last_byte = buf[to_send_sz - 1];

            uint16_t padded_word = (static_cast<uint16_t>(last_byte) << 8);

            accumulated_sum += padded_word;
        }

        return static_cast<uint16_t>(fold_and_accumulate(accumulated_sum));
    }

    void send_packet_update_state()
    {
        uint32_t sz = get_to_send_size();
        cur_seq_no = (cur_seq_no + sz) % MAX_SEQUENCE_NUMBER;
        can_be_sent_start = (can_be_sent_start + sz) % buf_size;
    }

public:
    send_window(char *buffer = new char[DEFAULT_BUFFER_SIZE], const size_t &buffer_size = DEFAULT_BUFFER_SIZE) : buf(buffer), buf_size(buffer_size), rcv_window_size(0), sent_not_acked_start(0),
                                                                                                                 can_be_sent_start(0), can_be_written_start(0), cur_seq_no(0), cur_ack_no(0) {}
    uint32_t send_packet(char *buf, const uint32_t &sz)
    {
        uint32_t toretsz = HEADER_SIZE + get_to_send_size();

        uint16_t checksum_left = serialize_header(buf, sz);
        uint16_t checksum_right = serialize_payload(buf + HEADER_SIZE, sz - HEADER_SIZE);

        uint16_t final_sum_folded = static_cast<uint16_t>(
            fold_and_accumulate(uint32_t(checksum_left) + checksum_right));

        uint16_t checksum = htons(~final_sum_folded);

        uint16_t chekcsum_offset = 12;
        memcpy(buf + chekcsum_offset, &checksum, sizeof(uint16_t));

        send_packet_update_state();

        return toretsz;
    }
};

//  ℹ️ resize buffer and all that can be added

class receive_window_sack_logic
{
public:
    // Stores [Start, End) of received out-of-order blocks
    using sequence_number = uint32_t;

    std::map<sequence_number, sequence_number> sack_blocks;

    // Returns true if the state changed
    void incoming_segment(sequence_number start_seq, sequence_number end_seq, sequence_number &rcv_nxt)
    {
        if (start_seq < rcv_nxt)
            start_seq = rcv_nxt;
        if (start_seq >= end_seq)
            return;

        sequence_number new_start = start_seq;
        sequence_number new_end = end_seq;

        // 1. Find insertion point and merge with neighbors
        auto it = sack_blocks.upper_bound(new_start);
        if (it != sack_blocks.begin())
        {
            --it;
            if (it->second < new_start)
                ++it;
        }

        std::vector<sequence_number> to_erase;
        auto current = it;

        while (current != sack_blocks.end() && current->first <= new_end)
        {
            if (current->second >= new_start)
            {
                new_start = std::min(new_start, current->first);
                new_end = std::max(new_end, current->second);
                to_erase.push_back(current->first);
            }
            ++current;
        }

        for (sequence_number key : to_erase)
            sack_blocks.erase(key);

        sack_blocks[new_start] = new_end;

        // 2. Advance RCV.NXT if the first block is now contiguous
        check_and_advance_rcv_nxt(rcv_nxt);
    }

private:
    void check_and_advance_rcv_nxt(sequence_number &rcv_nxt)
    {
        if (sack_blocks.empty())
            return;

        auto first = sack_blocks.begin();
        if (first->first == rcv_nxt)
        {
            rcv_nxt = first->second;
            sack_blocks.erase(first);
            check_and_advance_rcv_nxt(rcv_nxt);
        }
    }
};

class receive_window
{
    struct header
    {
        uint32_t seq_no;
        uint32_t ack_no;
        uint16_t win_sz;
        uint16_t flags; // 00000... syn ack fin rst [ 0s are reserved ]
        uint16_t checksum;
    };
    // 14 bytes
    static constexpr uint16_t HEADER_SIZE = 14;
    static constexpr uint32_t DEFAULT_BUFFER_SIZE = 4096;
    static constexpr uint16_t MAX_MESSAGE_TRANSMISSION_SIZE = 1400;
    static constexpr uint32_t MAX_SEQUENCE_NUMBER = (1ll << 32) - 1;

    char *buf;
    uint32_t buf_size;
    uint16_t rcv_window_size;
    uint32_t can_read_start;
    uint32_t not_acked_start;

    uint32_t can_be_written_start; // this is can be ack_no

    receive_window_sack_logic sack_handler;

    //
    uint32_t read_from_buffer(const char *ibuf, const uint32_t &sz)
    {
    }

    uint32_t fold_and_accumulate(uint32_t sum)
    {
        while (sum >> 16)
            sum = (sum & 0xFFFF) + (sum >> 16);
        return sum;
    }

    uint16_t deserialize_header(char *buf, const uint32_t &sz, header &h)
    {
        uint32_t accumulated_sum = 0;

        h.seq_no = *reinterpret_cast<uint32_t *>(buf);
        h.ack_no = *reinterpret_cast<uint32_t *>(buf + 4);
        h.win_sz = *reinterpret_cast<uint16_t *>(buf + 8);
        h.flags = *reinterpret_cast<uint16_t *>(buf + 10);
        h.checksum = *reinterpret_cast<uint16_t *>(buf + 12);

        for (uint32_t i = 0; i < 7; ++i)
        {
            uint32_t offset = i * 2;
            uint16_t high_byte = static_cast<uint8_t>(buf[offset]);
            uint16_t low_byte = static_cast<uint8_t>(buf[offset + 1]);

            accumulated_sum += (high_byte << 8) | low_byte;
        }

        h.seq_no = ntohl(h.seq_no);
        h.ack_no = ntohl(h.ack_no);
        h.win_sz = ntohs(h.win_sz);
        h.flags = ntohs(h.flags);
        h.checksum = ntohs(h.checksum);

        return static_cast<uint16_t>(fold_and_accumulate(accumulated_sum));
    }

    uint16_t deserialize_payload(char *ibuf, const uint32_t &sz, const uint32_t &start_offset)
    {
        uint32_t accumulated_sum = 0;

        uint32_t num_16bit_words = sz / 2;

        for (uint32_t i = 0; i < num_16bit_words; ++i)
        {
            uint32_t offset = i * 2 + start_offset;
            uint16_t high_byte = static_cast<uint8_t>(ibuf[offset]);
            uint16_t low_byte = static_cast<uint8_t>(ibuf[offset + 1]);

            accumulated_sum += (high_byte << 8) | low_byte;

            buf[(offset) % buf_size] = static_cast<char>(high_byte);
            buf[(offset + 1) % buf_size] = static_cast<char>(low_byte);
        }

        if (sz & 1)
        {
            uint8_t last_byte = ibuf[sz - 1];

            uint16_t padded_word = (static_cast<uint16_t>(last_byte) << 8);

            buf[(start_offset + sz - 1) % buf_size] = static_cast<char>(last_byte);

            accumulated_sum += padded_word;
        }

        return static_cast<uint16_t>(fold_and_accumulate(accumulated_sum));
    }

    // public:
    receive_window(char *buffer = new char[DEFAULT_BUFFER_SIZE], const size_t &buffer_size = DEFAULT_BUFFER_SIZE) : buf(buffer), buf_size(buffer_size), rcv_window_size(buffer_size), can_read_start(get_random_uint64_t() % MAX_SEQUENCE_NUMBER),
                                                                                                                    not_acked_start(can_read_start), can_be_written_start(can_read_start) {}
    uint32_t receive_packet(char *buf, const uint32_t &sz)
    {

        if (sz > rcv_window_size)
        {
            //  send back flow control info of new window and discard this packet
            return 0;
        }

        header h;
        uint16_t checksum_left = deserialize_header(buf, sz, h);
        uint16_t checksum_right = deserialize_payload(buf + HEADER_SIZE, sz - HEADER_SIZE, h.seq_no);

        uint16_t checksum = static_cast<uint16_t>((fold_and_accumulate(uint32_t(checksum_left) + checksum_right)));

        if (checksum != 0xffff)
            return 0;

        sack_handler.incoming_segment(h.seq_no, h.seq_no + sz, can_be_written_start);

        uint32_t highest_received_seq = can_be_written_start;
        if (!sack_handler.sack_blocks.empty())
            highest_received_seq = sack_handler.sack_blocks.rbegin()->second;

        uint32_t window_span = (can_read_start - highest_received_seq + buf_size) % buf_size;
        rcv_window_size = static_cast<uint16_t>(window_span);

        return sz;
    }
};

class reliable_ordered_channel : public channel
{

    struct header
    {
        uint32_t seq_no;
        uint32_t ack_no;
        uint16_t win_sz;
        uint16_t flags; // 00000... syn ack fin rst [ 0s are reserved ]
        uint16_t checksum;
    };
    // 14 bytes

    static constexpr size_t HEADER_SIZE = 14;
    //
    // protocol
    send_window snd_window;

    //

public:
    reliable_ordered_channel(channel_id id, channel_type type) : channel(id, type) {}

    // recieve from client what came on udp
    void on_transport_receive(std::unique_ptr<i_packet> pkt) override
    {
    }

    //
    std::unique_ptr<i_packet> on_transport_send() override
    {
        std::unique_ptr<raw_packet> pkt = std::make_unique<raw_packet>(1500); // ℹ️ can be optimized

        size_t offset = rudp_protocol::CHANNEL_HEADER_OFFSET;

        uint32_t len = snd_window.send_packet(pkt->get_buffer() + offset, pkt->get_capacity() - offset);

        pkt->set_length(offset + len);

        return pkt;
    }

    // raed into application buffer what came from network
    ssize_t read_bytes_to_application(char *buf, const size_t &len) override {}

    // coming from application to send to network
    ssize_t write_bytes_from_application(const char *buf, const size_t &len) override
    {
    }
};