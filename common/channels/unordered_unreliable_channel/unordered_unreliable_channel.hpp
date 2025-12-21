#pragma once

#include "../../i_channel.hpp"
#include "../../types.hpp"
#include "../../logger.hpp"
#include "../../rudp_protocol_packet.hpp"

#include <queue>
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
        constexpr uint32_t DEFAULT_BUFFER_SIZE = 64 * 1024;
        constexpr uint16_t MAX_MSS = 65000;
    }

    struct channel_header
    {
    };

    // ---------------- PACKET CODEC ----------------

    class packet_codec
    {
    public:
        static bool deserialize_header(const char *, uint32_t, channel_header &)
        {
            return true;
        }

        static void serialize_header(char *, const channel_header &)
        {
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

        ssize_t read_data(char *obuf, uint32_t sz)
        {
            if (data.empty())
                return 0;

            auto &pkt = data.front();
            uint32_t off = rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            uint32_t packet_size = pkt->get_length() - off;

            uint32_t toread = std::min(packet_size, sz);
            std::memcpy(obuf, pkt->get_buffer() + off, toread);

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
                rudp_protocol_packet::CHANNEL_HEADER_OFFSET;

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
                rudp_protocol_packet::CHANNEL_HEADER_OFFSET;

            bytes_avail -= payload;
            return ret;
        }
    };

    // ---------------- SEND WINDOW (NO LOCKS) ----------------

    class send_window
    {
        channel_buffer buffer;

    public:
        explicit send_window(uint32_t size = channel_config::DEFAULT_BUFFER_SIZE)
            : buffer(size) {}

        bool receive_bytes(const char *ibuf, uint32_t sz)
        {
            if (sz == 0)
                return false;

            size_t off = rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            uint32_t len = off + sz;

            auto pkt = std::make_unique<rudp_protocol_packet>(len);
            pkt->set_length(len);
            std::memcpy(pkt->get_buffer() + off, ibuf, sz);

            return buffer.put_rudp_protocol_packet(std::move(pkt));
        }

        uint32_t get_available_bytes_cnt() const
        {
            return buffer.get_bytes_avail();
        }

        std::unique_ptr<rudp_protocol_packet> get_rudp_protocol_pkt()
        {
            return buffer.get_rudp_protocol_packet();
        }
    };

    // ---------------- RECEIVE WINDOW (NO LOCKS) ----------------

    class receive_window
    {
        channel_buffer buffer;

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
        send_window snd_window;
        receive_window rcv_window;

        std::function<void()> on_app_data_ready;
        std::function<void(std::unique_ptr<rudp_protocol_packet>)> on_net_data_ready;

        mutable std::mutex mutex_; // SINGLE MUTEX

        std::unique_ptr<rudp_protocol_packet> on_transport_send_nolock()
        {
            auto pkt = snd_window.get_rudp_protocol_pkt();
            if (!pkt)
                return nullptr;

            char *pkt_buf = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            channel_header h{};
            packet_codec::serialize_header(pkt_buf, h);
            return pkt;
        }

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

            channel_header header{};
            if (!rcv_window.receive_packet(std::move(pkt), header))
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
            // Not required
        }
    };
}
