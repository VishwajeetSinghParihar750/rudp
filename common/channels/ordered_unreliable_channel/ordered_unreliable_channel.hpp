#pragma once

#include "../../i_channel.hpp"
#include "../../types.hpp"
#include "../../logger.hpp"
#include "../../rudp_protocol_packet.hpp"

#include <deque>
#include <cstring>
#include <algorithm>
#include <memory>
#include <functional>
#include <cstdint>
#include <chrono>
#include <arpa/inet.h>
#include <atomic>
#include <mutex>
#include <cassert>
#include <string>

class i_timer_service;

namespace ordered_unreliable_channel
{
    namespace channel_config
    {
        constexpr uint16_t HEADER_SIZE = 5;
        constexpr uint32_t DEFAULT_BUFFER_SIZE = 1024 * 1024;
        constexpr uint16_t MAX_MSS = 65000;
    }

    enum class channel_header_flags : uint8_t
    {
        CYCLE = 1,
    };

    struct channel_header
    {
        uint32_t seq_no;
        uint8_t flags;
    };

    // ---------------- PACKET CODEC ----------------

    class packet_codec
    {
    public:
        static bool deserialize_header(const char *buf, uint32_t total_size, channel_header &out_header)
        {
            if (total_size < sizeof(channel_header))
                return false;

            // Manual memcpy to avoid strict aliasing / alignment issues
            uint32_t seq;
            memcpy(&seq, buf, sizeof(seq));
            out_header.seq_no = ntohl(seq);
            out_header.flags = *(buf + sizeof(uint32_t));
            return true;
        }

        static void serialize_header(char *buf, const channel_header &header)
        {
            uint32_t seq = htonl(header.seq_no);
            memcpy(buf, &seq, sizeof(seq));
            memcpy(buf + sizeof(seq), &header.flags, sizeof(header.flags));
        }
    };

    // ---------------- CHANNEL BUFFER (SINGLE COPY IMPLEMENTATION) ----------------

    class channel_buffer
    {
        std::deque<std::unique_ptr<rudp_protocol_packet>> data;
        uint32_t size;
        uint32_t bytes_avail = 0;

        // SINGLE COPY STATE: Track offset within the front packet
        uint32_t head_read_offset = 0;

    public:
        explicit channel_buffer(uint32_t max_bytes)
            : size(max_bytes) {}

        uint32_t get_bytes_avail() const
        {
            return bytes_avail - head_read_offset;
        }

        ssize_t read_data(char *obuf, uint32_t sz)
        {
            uint32_t total_read = 0;

            while (sz > 0 && !data.empty())
            {
                auto &pkt = data.front();
                
                uint32_t header_reserve = rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;
                uint32_t pkt_len = pkt->get_length();
                
                // Safety check for malformed packets smaller than header
                if (pkt_len <= header_reserve) {
                    data.pop_front();
                    head_read_offset = 0;
                    continue; 
                }

                uint32_t packet_payload_len = pkt_len - header_reserve;

                // Calculate available bytes in this specific packet
                uint32_t available_in_pkt = packet_payload_len - head_read_offset;

                uint32_t to_copy = std::min(available_in_pkt, sz);

                // Direct memcpy from packet payload area + read offset
                memcpy(obuf, pkt->get_buffer() + header_reserve + head_read_offset, to_copy);

                // Update pointers and counters
                obuf += to_copy;
                sz -= to_copy;
                total_read += to_copy;
                head_read_offset += to_copy;

                // If packet is fully consumed, pop it
                if (head_read_offset >= packet_payload_len)
                {
                    bytes_avail -= packet_payload_len;
                    data.pop_front();
                    head_read_offset = 0; // Reset for next packet
                }
            }
            return total_read;
        }

        bool put_rudp_protocol_packet(std::unique_ptr<rudp_protocol_packet> pkt)
        {
            if (!pkt) return false;

            uint32_t pkt_len = pkt->get_length();
            uint32_t reserve = rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;

            if (pkt_len <= reserve) return false;

            uint32_t payload = pkt_len - reserve;

            if (get_bytes_avail() + payload > size)
                return false;

            data.push_back(std::move(pkt));
            bytes_avail += payload;
            return true;
        }
    };

    // ---------------- SEND STATE (NO QUEUE) ----------------

    class send_state
    {
        uint32_t seq_no = 0;
        bool cycle = false;

    public:
        // Returns the sequence number to use for the next packet of 'payload_len'
        // and updates the internal state.
        void get_next_header(uint32_t payload_len, channel_header &out_header)
        {
            out_header.seq_no = seq_no;
            out_header.flags = 0;

            if (UINT32_MAX - seq_no < payload_len)
            {
                cycle = !cycle;
            }
            
            if (cycle)
            {
                out_header.flags |= uint8_t(channel_header_flags::CYCLE);
            }

            seq_no += payload_len;
        }
    };

    // ---------------- RECEIVE WINDOW (NO LOCKS) ----------------

    class receive_window
    {
        channel_buffer buffer;
        bool last_cycle = false;
        uint32_t next_expected_seqno = 0;

    public:
        explicit receive_window(uint32_t size = channel_config::DEFAULT_BUFFER_SIZE)
            : buffer(size) {}

        bool receive_packet(std::unique_ptr<rudp_protocol_packet> pkt)
        {
            if (!pkt) return false;

            char *packet_ptr = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            uint32_t packet_size = pkt->get_length() - rudp_protocol_packet::CHANNEL_HEADER_OFFSET;

            channel_header h{};
            if (!packet_codec::deserialize_header(packet_ptr, packet_size, h))
                return false;

            bool new_cycle = (h.flags & uint8_t(channel_header_flags::CYCLE)) != 0;

            // Ordered Unreliable Logic:
            // We accept if Sequence is NEWER than expected (gap/jump ahead)
            // OR if Cycle flag changed (wrap around)
            
            // Note: simple (seq >= expected) check fails on wrap-around boundary without cycle flag logic.
            // The Cycle flag handles the uint32 overflow ambiguity.
            
            bool is_newer = false;
            if (new_cycle != last_cycle)
            {
                is_newer = true; // Cycle changed, must be new data
            }
            else
            {
                // Same cycle, strictly strictly greater or equal
                is_newer = (h.seq_no >= next_expected_seqno);
            }

            if (is_newer)
            {
                uint32_t payload_len = packet_size - channel_config::HEADER_SIZE;
                
                // Try to buffer
                if (buffer.put_rudp_protocol_packet(std::move(pkt)))
                {
                    // Update state to expect the NEXT byte after this packet
                    last_cycle = new_cycle;
                    next_expected_seqno = h.seq_no + payload_len;
                    return true;
                }
            }

            return false;
        }

        uint32_t get_available_bytes_cnt() const
        {
            return buffer.get_bytes_avail();
        }

        ssize_t read_data(char *out_buf, uint32_t len)
        {
            return buffer.read_data(out_buf, len);
        }
    };

    // ---------------- ORDERED UNRELIABLE CHANNEL ----------------

    class ordered_unreliable_channel
        : public i_channel,
          public std::enable_shared_from_this<ordered_unreliable_channel>
    {
        channel_id ch_id;
        
        // We replaced send_window with lightweight state since we don't queue
        send_state snd_state; 
        receive_window rcv_window;

        std::function<void()> on_app_data_ready;
        std::function<void(std::shared_ptr<rudp_protocol_packet>)> on_net_data_ready;

        mutable std::mutex mutex_; // SINGLE MUTEX

    public:
        explicit ordered_unreliable_channel(channel_id id)
            : ch_id(id)
        {
            LOG_INFO("Ordered Unreliable Channel " << ch_id << " created");
        }

        std::unique_ptr<i_channel> clone() const override
        {
            return nullptr;
        }

        void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
        {
            if (!pkt) return;

            std::lock_guard<std::mutex> lock(mutex_);

            if (rcv_window.receive_packet(std::move(pkt)))
            {
                if (on_app_data_ready) on_app_data_ready();
            }
        }

        ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ssize_t ret = rcv_window.read_data(buf, len);
            
            // If more data remains, notify app again
            if (rcv_window.get_available_bytes_cnt() > 0 && on_app_data_ready)
                on_app_data_ready();
                
            return ret;
        }

        ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
        {
            // Direct Send Optimization (No Queue)
            
            uint32_t total_len = len + rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;
            
            // 1. Alloc Packet
            auto pkt = std::make_unique<rudp_protocol_packet>(total_len);
            pkt->set_length(total_len);
            
            {
                std::lock_guard<std::mutex> lock(mutex_);
                
                // 2. Generate Header & Seq No
                channel_header h{};
                snd_state.get_next_header(len, h);
                
                // 3. Serialize Header
                char *header_ptr = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
                packet_codec::serialize_header(header_ptr, h);
                
                // 4. Copy Payload
                memcpy(header_ptr + channel_config::HEADER_SIZE, buf, len);
            }

            // 5. Send (Outside Lock)
            if (on_net_data_ready)
                on_net_data_ready(std::move(pkt));

            return len;
        }

        void set_on_app_data_ready(std::function<void()> f) override { on_app_data_ready = f; }
        void set_on_net_data_ready(std::function<void(std::shared_ptr<rudp_protocol_packet>)> f) override { on_net_data_ready = f; }
        void set_timer_service(std::shared_ptr<i_timer_service>) override { /* Not used */ }
    };
}