#pragma once

#include "../../i_channel.hpp"
#include "../../types.hpp"
#include "../../logger.hpp"
#include "../../rudp_protocol_packet.hpp"

#include <deque> // Changed to deque for standard compliance with this pattern
#include <cstring>
#include <algorithm>
#include <memory>
#include <functional>
#include <cstdint>
#include <cassert>
#include <string>
#include <mutex>

class i_timer_service;

namespace unordered_unreliable_channel
{
    namespace channel_config
    {
        constexpr uint16_t HEADER_SIZE = 0;
        constexpr uint32_t DEFAULT_BUFFER_SIZE = 1 * 1024 * 1024;
    }

    // ---------------- CHANNEL BUFFER (SINGLE COPY IMPLEMENTATION) ----------------

    class channel_buffer
    {
        std::deque<std::unique_ptr<rudp_protocol_packet>> data;
        uint32_t size;
        uint32_t bytes_avail = 0;

        // SINGLE COPY STATE: Tracks how much we have read from data.front()
        uint32_t head_read_offset = 0;

    public:
        explicit channel_buffer(uint32_t max_bytes)
            : size(max_bytes) {}

        uint32_t get_bytes_avail() const
        {
            // Total bytes available = Total payload bytes in queue - bytes already consumed from head
            return bytes_avail - head_read_offset;
        }

        ssize_t read_data(char *obuf, uint32_t sz)
        {
            uint32_t total_read = 0;

            while (sz > 0 && !data.empty())
            {
                auto &pkt = data.front();

                uint32_t header_reserve = rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;
                uint32_t packet_payload_len = pkt->get_length() - header_reserve;

                // How much is left in this specific packet?
                uint32_t available_in_pkt = packet_payload_len - head_read_offset;

                // Copy whichever is smaller: what user wants, or what's left in this packet
                uint32_t to_copy = std::min(available_in_pkt, sz);

                // Direct memcpy from packet buffer + header offset + current read offset
                std::memcpy(obuf, pkt->get_buffer() + header_reserve + head_read_offset, to_copy);

                // Advance pointers
                obuf += to_copy;
                sz -= to_copy;
                total_read += to_copy;
                head_read_offset += to_copy;

                // If we have fully consumed the front packet, pop it
                if (head_read_offset >= packet_payload_len)
                {
                    bytes_avail -= packet_payload_len; // Remove total bytes count
                    data.pop_front();
                    head_read_offset = 0; // Reset for the next packet
                }
            }

            return total_read;
        }

        bool put_rudp_protocol_packet(std::unique_ptr<rudp_protocol_packet> pkt)
        {
            if (!pkt)
                return false;

            uint32_t payload =
                pkt->get_length() -
                rudp_protocol_packet::CHANNEL_HEADER_OFFSET - channel_config::HEADER_SIZE;

            if (payload == 0 || (get_bytes_avail() + payload) > size)
                return false;

            data.push_back(std::move(pkt));
            bytes_avail += payload;
            return true;
        }

        std::unique_ptr<rudp_protocol_packet> get_rudp_protocol_packet()
        {
            if (data.empty())
                return nullptr;

            auto ret = std::move(data.front());
            data.pop_front();

            // Note: If we are popping manually here, we assume the user wanted the WHOLE packet
            // regardless of the offset. However, usually read_data is used.
            // We subtract the full payload size from the tracker.
            uint32_t payload =
                ret->get_length() -
                rudp_protocol_packet::CHANNEL_HEADER_OFFSET - channel_config::HEADER_SIZE;

            bytes_avail -= payload;
            head_read_offset = 0; // Reset offset just in case
            return ret;
        }
    };

    // ---------------- RECEIVE WINDOW (NO LOCKS) ----------------

    class receive_window
    {
        channel_buffer buffer;

    public:
        explicit receive_window(uint32_t size = channel_config::DEFAULT_BUFFER_SIZE)
            : buffer(size) {}

        bool receive_packet(std::unique_ptr<rudp_protocol_packet> pkt)
        {
            if (!pkt)
                return false;

            // No codec, just raw checking offsets
            uint32_t off = rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;
            if (pkt->get_length() <= off)
                return false;

            return buffer.put_rudp_protocol_packet(std::move(pkt));
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

    // ---------------- UNORDERED UNRELIABLE CHANNEL ----------------

    class unordered_unreliable_channel
        : public i_channel,
          public std::enable_shared_from_this<unordered_unreliable_channel>
    {
        channel_id ch_id;
        receive_window rcv_window;

        std::function<void()> on_app_data_ready;
        std::function<void(std::shared_ptr<rudp_protocol_packet>)> on_net_data_ready;

        mutable std::mutex mutex_; // SINGLE MUTEX

    public:
        explicit unordered_unreliable_channel(channel_id id)
            : ch_id(id)
        {
            LOG_INFO("Unordered Unreliable Channel " << ch_id << " created.");
        }

        std::unique_ptr<i_channel> clone() const override
        {
            return nullptr;
        }

        void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
        {
            if (!pkt)
                return;

            std::lock_guard<std::mutex> lock(mutex_);

            if (!rcv_window.receive_packet(std::move(pkt)))
                return;

            if (rcv_window.get_available_bytes_cnt() > 0 && on_app_data_ready)
                on_app_data_ready();
        }

        ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
        {
            std::lock_guard<std::mutex> lock(mutex_);

            ssize_t ret = rcv_window.read_data(buf, len);
            if (rcv_window.get_available_bytes_cnt() > 0 && on_app_data_ready)
                on_app_data_ready();
            return ret;
        }

        ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
        {
            // Direct send, no windowing for unreliable
            auto pkt = std::make_unique<rudp_protocol_packet>(len + rudp_protocol_packet::CHANNEL_HEADER_OFFSET);
            pkt->set_length(len + rudp_protocol_packet::CHANNEL_HEADER_OFFSET);

            // Copy data to payload area
            std::memcpy(pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET, buf, len);

            if (on_net_data_ready)
                on_net_data_ready(std::move(pkt));

            return len;
        }

        void set_on_app_data_ready(std::function<void()> f) override
        {
            on_app_data_ready = f;
        }

        void set_on_net_data_ready(std::function<void(std::shared_ptr<rudp_protocol_packet>)> f) override
        {
            on_net_data_ready = f;
        }

        void set_timer_service(std::shared_ptr<i_timer_service>) override
        {
            // Not required
        }
    };
}