#pragma once

#include "../../i_channel.hpp"
#include "../../types.hpp"
#include "../../logger.hpp"
#include "../../rudp_protocol_packet.hpp"

#include <map>
#include <deque>
#include <queue>
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
#include <stdexcept>
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

    // ---------------- CHANNEL BUFFER (NO LOCKS) ----------------

    class channel_buffer
    {
        std::queue<std::unique_ptr<rudp_protocol_packet>> data;
        uint32_t size;
        uint32_t bytes_avail = 0;

    public:
        explicit channel_buffer(uint32_t max_bytes)
            : size(max_bytes) {}

        uint32_t get_bytes_avail() const
        {
            return bytes_avail;
        }

        ssize_t read_data(char *obuf, const uint32_t &sz)
        {
            if (data.empty())
                return 0;

            auto &pkt = data.front();
            uint32_t off = rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;
            uint32_t packet_size = pkt->get_length() - off;

            uint32_t toread = std::min(packet_size, sz);
            
            memcpy(obuf, pkt->get_buffer() + off, toread);

            data.pop();
            bytes_avail -= packet_size;
            return toread;
        }

        bool put_rudp_protocol_packet(std::unique_ptr<rudp_protocol_packet> pkt)
        {
            if (!pkt)
                return false;

            uint32_t payload =
                pkt->get_length() -
                rudp_protocol_packet::CHANNEL_HEADER_OFFSET -
                channel_config::HEADER_SIZE;

            if (payload == 0 || bytes_avail + payload > size)
                return false;

            data.push(std::move(pkt));
            bytes_avail += payload;
            return true;
        }

        std::unique_ptr<rudp_protocol_packet> get_rudp_protocol_packet()
        {
            if (data.empty())
                return nullptr;

            auto ret = std::move(data.front());
            data.pop();

            uint32_t payload =
                ret->get_length() -
                rudp_protocol_packet::CHANNEL_HEADER_OFFSET -
                channel_config::HEADER_SIZE;

            bytes_avail -= payload;
            return ret;
        }
    };

    // ---------------- SEND WINDOW (NO LOCKS) ----------------

    class send_window
    {
        channel_buffer buffer;
        uint32_t seq_no = 0;
        bool cycle = false;

    public:
        explicit send_window(uint32_t size = channel_config::DEFAULT_BUFFER_SIZE)
            : buffer(size) {}

        bool receive_bytes(const char *ibuf, const uint32_t &sz)
        {
            if (sz == 0)
                return false;

            size_t off = rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;
            uint32_t len = off + sz;

            auto pkt = std::make_unique<rudp_protocol_packet>(len);
            pkt->set_length(len);
            memcpy(pkt->get_buffer() + off, ibuf, sz);

            return buffer.put_rudp_protocol_packet(std::move(pkt));
        }

        uint32_t get_available_bytes_cnt() const
        {
            return buffer.get_bytes_avail();
        }

        std::unique_ptr<rudp_protocol_packet> get_rudp_protocol_pkt(uint32_t &seq_no_out, bool &cycle_out)
        {
            seq_no_out = seq_no;
            cycle_out = cycle;

            auto pkt = buffer.get_rudp_protocol_packet();
            if (!pkt)
                return nullptr;

            uint32_t advance = pkt->get_length() - rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            if (UINT32_MAX - seq_no < advance)
                cycle = !cycle;

            seq_no += advance;
            return pkt;
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

        bool receive_packet(std::unique_ptr<rudp_protocol_packet> pkt, channel_header &out_header)
        {
            if (!pkt)
                return false;

            char *packet = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            uint32_t packet_size = pkt->get_length() - rudp_protocol_packet::CHANNEL_HEADER_OFFSET;

            if (!packet_codec::deserialize_header(packet, packet_size, out_header))
                return false;

            bool new_cycle = (out_header.flags & uint8_t(channel_header_flags::CYCLE)) != 0;

            if (out_header.seq_no >= next_expected_seqno || new_cycle != last_cycle)
            {
                if (buffer.put_rudp_protocol_packet(std::move(pkt)))
                {
                    last_cycle = new_cycle;
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
        send_window snd_window;
        receive_window rcv_window;

        std::function<void()> on_app_data_ready;
        std::function<void(std::unique_ptr<rudp_protocol_packet>)> on_net_data_ready;

        mutable std::mutex mutex_; // SINGLE MUTEX

        std::unique_ptr<rudp_protocol_packet> on_transport_send_nolock()
        {
            channel_header header{};
            bool cycle = false;

            auto pkt = snd_window.get_rudp_protocol_pkt(header.seq_no, cycle);
            if (!pkt)
                return nullptr;

            if (cycle)
                header.flags |= uint8_t(channel_header_flags::CYCLE);

            char *pkt_buf = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            packet_codec::serialize_header(pkt_buf, header);
            return pkt;
        }

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
            if (!pkt)
                return;

            std::lock_guard<std::mutex> lock(mutex_);

            channel_header header{};
            if (rcv_window.receive_packet(std::move(pkt), header))
                on_app_data_ready();
        }

        ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
        {
            std::lock_guard<std::mutex> lock(mutex_);

            ssize_t ret = rcv_window.read_data(buf, len);
            return ret;
        }

        ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
        {
            std::unique_ptr<rudp_protocol_packet> pkt;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!snd_window.receive_bytes(buf, len))
                    return 0;

                pkt = on_transport_send_nolock();
            }

            if (pkt && on_net_data_ready)
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
            // Not used
        }
    };
}
