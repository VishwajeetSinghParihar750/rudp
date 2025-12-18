#pragma once

#include "../../i_channel.hpp"
#include "../../rudp_protocol_packet.hpp"
#include "../../types.hpp"
#include "../../logger.hpp"

#include <map>
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
#include <stdexcept>
#include <string>

class i_timer_service;

namespace ordered_unreliable_channel
{
    namespace channel_config
    {
        constexpr uint16_t HEADER_SIZE = 5;
        constexpr uint32_t DEFAULT_BUFFER_SIZE = 32 * 1024 * 1024;
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

    class packet_codec
    {
    public:
        static bool deserialize_header(const char *buf, uint32_t total_size, channel_header &out_header)
        {
            if (total_size < sizeof(channel_header))
                return false;

            out_header.seq_no = *reinterpret_cast<const uint32_t *>(buf);
            out_header.seq_no = ntohl(out_header.seq_no);
            out_header.flags = *reinterpret_cast<const uint8_t *>(buf + sizeof(channel_header::seq_no));

            return true;
        }

        static void serialize_header(char *buf, const channel_header &header)
        {
            uint32_t seq_no = htonl(header.seq_no);
            memcpy(buf, &seq_no, sizeof(seq_no));
            memcpy(buf + sizeof(channel_header::seq_no), &header.flags, sizeof(header.flags));
        }
    };

    class channel_buffer
    {
        std::queue<std::unique_ptr<rudp_protocol_packet>> data;
        uint32_t size;
        uint32_t bytes_avail = 0;
        mutable std::mutex buffer_mutex;

    public:
        channel_buffer(uint32_t max_bytes) : size(max_bytes) {}

        uint32_t get_free_space() const
        {
            std::lock_guard<std::mutex> guard(buffer_mutex);
            return size - bytes_avail;
        }
        uint32_t get_bytes_avail() const
        {
            std::lock_guard<std::mutex> guard(buffer_mutex);
            return bytes_avail;
        }

        ssize_t read_data(char *obuf, const uint32_t &sz)
        {
            std::lock_guard<std::mutex> guard(buffer_mutex);
            if (data.empty())
                return 0;

            auto &pkt = data.front();
            uint32_t off = rudp_protocol_packet::CHANNEL_HEADER_OFFSET + channel_config::HEADER_SIZE;
            uint32_t packet_size = pkt->get_length() - off;
            uint32_t bytes_pending = packet_size;

            uint32_t toread = std::min(bytes_pending, sz);
            memcpy(obuf, pkt->get_buffer() + off, toread);

            data.pop();
            bytes_avail -= packet_size;
            return toread;
        }

        bool put_rudp_protocol_packet(std::unique_ptr<rudp_protocol_packet> pkt)
        {
            std::lock_guard<std::mutex> guard(buffer_mutex);
            if (pkt == nullptr)
                return false;

            uint32_t packet_size = pkt->get_length() - (rudp_protocol_packet::CHANNEL_HEADER_OFFSET)-channel_config::HEADER_SIZE;
            if (packet_size == 0 || (bytes_avail + packet_size > size))
                return false;

            data.push(std::move(pkt));
            bytes_avail += packet_size;
            return true;
        }
        std::unique_ptr<rudp_protocol_packet> get_rudp_protocol_packet()
        {
            std::lock_guard<std::mutex> guard(buffer_mutex);
            if (data.empty())
                return nullptr;
            auto ret = std::move(data.front());
            data.pop();

            uint32_t packet_size = ret->get_length() - (rudp_protocol_packet::CHANNEL_HEADER_OFFSET)-channel_config::HEADER_SIZE;
            if (bytes_avail >= packet_size)
                bytes_avail -= packet_size;

            return ret;
        }
    };

    class send_window
    {
    private:
        channel_buffer buffer;
        uint32_t seq_no = 0;
        bool cycle = false;
        mutable std::mutex lock;

    public:
        explicit send_window(uint32_t size = channel_config::DEFAULT_BUFFER_SIZE)
            : buffer(size) {}

        bool receive_bytes(const char *ibuf, const uint32_t &sz)
        {
            if (sz == 0)
                return false;

            size_t off = (rudp_protocol_packet::CHANNEL_HEADER_OFFSET) + channel_config::HEADER_SIZE;
            uint32_t len = off + sz;
            std::unique_ptr<rudp_protocol_packet> pkt = std::make_unique<rudp_protocol_packet>(len);
            pkt->set_length(len);
            memcpy(pkt->get_buffer() + off, ibuf, sz);

            return receive_packet(std::move(pkt));
        }
        bool receive_packet(std::unique_ptr<rudp_protocol_packet> pkt)
        {
            std::lock_guard<std::mutex> guard(lock);
            if (pkt == nullptr)
                return false;
            return buffer.put_rudp_protocol_packet(std::move(pkt));
        }

        uint32_t get_available_bytes_cnt()
        {
            std::lock_guard<std::mutex> guard(lock);
            return buffer.get_bytes_avail();
        }

        std::unique_ptr<rudp_protocol_packet> get_rudp_protocol_pkt(uint32_t &seq_no_, bool &cycle_)
        {
            std::lock_guard<std::mutex> guard(lock);
            seq_no_ = seq_no;
            cycle_ = cycle;
            auto toret = buffer.get_rudp_protocol_packet();
            if (toret == nullptr)
                return nullptr;

            uint32_t advance = toret->get_length() - rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            if (UINT32_MAX - seq_no < advance)
                cycle = !cycle;
            seq_no += advance;

            return toret;
        }
    };

    class receive_window
    {
    private:
        channel_buffer buffer;
        bool last_cycle = false;
        uint32_t next_expected_seqno = 0;
        mutable std::mutex lock;

    public:
        explicit receive_window(uint32_t size = channel_config::DEFAULT_BUFFER_SIZE)
            : buffer(size) {}

        bool receive_packet(std::unique_ptr<rudp_protocol_packet> pkt, channel_header &out_header)
        {
            std::lock_guard<std::mutex> guard(lock);
            if (pkt == nullptr)
                return false;

            char *packet = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            uint32_t packet_size = pkt->get_length() - (rudp_protocol_packet::CHANNEL_HEADER_OFFSET);

            if (!packet_codec::deserialize_header(packet, packet_size, out_header))
            {
                LOG_WARN("[receive_window::receive_packet] Packet failed header deserialization (checksum fail or malformed).");
                return false;
            }

            bool new_cycle = ((out_header.flags & uint8_t(channel_header_flags::CYCLE)) & 1);
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

        uint32_t get_available_bytes_cnt()
        {
            std::lock_guard<std::mutex> guard(lock);
            return buffer.get_bytes_avail();
        }

        ssize_t read_data(char *out_buf, uint32_t len)
        {
            std::lock_guard<std::mutex> guard(lock);
            return buffer.read_data(out_buf, len);
        }
    };

    class ordered_unreliable_channel : public i_channel, public std::enable_shared_from_this<ordered_unreliable_channel>
    {
    private:
        channel_id ch_id;
        send_window snd_window;
        receive_window rcv_window;
        std::function<void()> on_app_data_ready;
        std::function<void(std::unique_ptr<rudp_protocol_packet>)> on_net_data_ready;

        std::unique_ptr<rudp_protocol_packet> on_transport_send()
        {
            channel_header header{};
            bool cycle;

            auto pkt = snd_window.get_rudp_protocol_pkt(header.seq_no, cycle);

            if (pkt == nullptr)
                return nullptr;

            if (cycle)
                header.flags |= (uint8_t)(channel_header_flags::CYCLE);

            char *pkt_buf = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            packet_codec::serialize_header(pkt_buf, header);

            return pkt;
        }

    public:
        explicit ordered_unreliable_channel(channel_id id) : ch_id(id)
        {
            LOG_INFO("[ordered_unreliable_channel::ordered_unreliable_channel] Unreliable Unordered Channel " << std::to_string(ch_id) << " created.");
        }

        std::unique_ptr<i_channel> clone() const override
        {
            LOG_ERROR("[ordered_unreliable_channel::clone] Cloning not supported rn ");
            return nullptr;
        }

        void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
        {
            if (!pkt)
                return;

            const size_t off = rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            const char *buf = pkt->get_const_buffer();
            size_t len = pkt->get_length();

            if (len <= off)
                return;

            const char *ibuf = buf + off;
            uint32_t sz = static_cast<uint32_t>(len - off);

            channel_header header{};
            if (!rcv_window.receive_packet(std::move(pkt), header))
            {
                LOG_WARN("[ordered_unreliable_channel::on_transport_receive] Failed to process received packet in receive window. Dropped.");
                return;
            }

            if (rcv_window.get_available_bytes_cnt() > 0)
            {
                LOG_INFO("[ordered_unreliable_channel::on_transport_receive] Received data payload. Notifying application.");
                if (on_app_data_ready)
                    on_app_data_ready();
            }
        }

        ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
        {
            auto ret = rcv_window.read_data(buf, len);
            if (rcv_window.get_available_bytes_cnt() > 0 && on_app_data_ready)
                on_app_data_ready();
            return ret;
        }

        ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
        {
            if (snd_window.receive_bytes(buf, len))
            {
                LOG_INFO("[ordered_unreliable_channel::write_bytes_from_application] Application wrote " << std::to_string(len) << " bytes. Attempting to send immediately.");
                auto pkt = on_transport_send();
                if (pkt && on_net_data_ready)
                    on_net_data_ready(std::move(pkt));
                return len;
            }
            LOG_WARN("[ordered_unreliable_channel::write_bytes_from_application] Application write failed (buffer full).");
            return 0;
        }

        void set_on_app_data_ready(std::function<void()> f) override
        {
            on_app_data_ready = f;
            LOG_TEST("[ordered_unreliable_channel::set_on_app_data_ready] on_app_data_ready callback set.");
        }

        void set_on_net_data_ready(std::function<void(std::unique_ptr<rudp_protocol_packet>)> f) override
        {
            on_net_data_ready = f;
            LOG_TEST("[ordered_unreliable_channel::set_on_net_data_ready] on_net_data_ready callback set.");
        }

        void set_timer_service(std::shared_ptr<i_timer_service> timer_man) override
        {
            LOG_INFO("[ordered_unreliable_channel::set_timer_manager] Timer manager not needed for unordered unreliable channel.");
        }
    };
}