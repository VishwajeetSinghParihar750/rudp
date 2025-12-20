#pragma once

#include "../../i_channel.hpp"
#include "../../types.hpp"
#include "../../rudp_protocol_packet.hpp"
#include "../../i_timer_service.hpp"
#include "../../timer_info.hpp"
#include "../../logger.hpp"

#include <map>
#include <deque>
#include <vector>
#include <cstring>
#include <memory>
#include <functional>
#include <cstdint>
#include <chrono>
#include <arpa/inet.h>
#include <mutex>
#include <algorithm>

namespace reliable_ordered_channel
{
    inline bool seq_lt(uint32_t a, uint32_t b)
    {
        return (int32_t)(a - b) < 0;
    }

    inline bool seq_le(uint32_t a, uint32_t b)
    {
        return (int32_t)(a - b) <= 0;
    }

    inline bool seq_gt(uint32_t a, uint32_t b)
    {
        return (int32_t)(a - b) > 0;
    }

    inline bool seq_ge(uint32_t a, uint32_t b)
    {
        return (int32_t)(a - b) >= 0;
    }

    namespace channel_config
    {
        constexpr uint16_t HEADER_SIZE = 14;
        constexpr uint32_t DEFAULT_BUFFER_SIZE = 65335;
        constexpr uint16_t MAX_MSS = 32 * 1024;
        constexpr uint64_t RTO_MS = 200;
        constexpr uint32_t MAX_RETRANSMITS = 5;
    }

    enum class channel_flags : uint16_t
    {
        ACK = 1,
        WIND_SZ = 2,
    };

    struct channel_header
    {
        uint32_t seq_no;
        uint32_t ack_no;
        uint16_t win_sz;
        uint16_t flags;
        uint16_t checksum;
    };

    /* ================= PACKET CODEC ================= */

    class packet_codec
    {
    public:
        static bool deserialize_header(const char *buf, uint32_t sz, channel_header &h)
        {
            if (sz < channel_config::HEADER_SIZE)
                return false;

            uint32_t seq, ack;
            uint16_t win, flg, csum;

            std::memcpy(&seq, buf + 0, 4);
            std::memcpy(&ack, buf + 4, 4);
            std::memcpy(&win, buf + 8, 2);
            std::memcpy(&flg, buf + 10, 2);
            std::memcpy(&csum, buf + 12, 2);

            h.seq_no = ntohl(seq);
            h.ack_no = ntohl(ack);
            h.win_sz = ntohs(win);
            h.flags = ntohs(flg);
            h.checksum = ntohs(csum);
            return true;
        }

        static void serialize_header(char *buf, const channel_header &h)
        {
            uint32_t seq = htonl(h.seq_no);
            uint32_t ack = htonl(h.ack_no);
            uint16_t win = htons(h.win_sz);
            uint16_t flg = htons(h.flags);
            uint16_t zero = 0;

            std::memcpy(buf + 0, &seq, 4);
            std::memcpy(buf + 4, &ack, 4);
            std::memcpy(buf + 8, &win, 2);
            std::memcpy(buf + 10, &flg, 2);
            std::memcpy(buf + 12, &zero, 2);
        }
    };

    /* ================= CIRCULAR BUFFER ================= */

    class circular_buffer
    {
        std::unique_ptr<char[]> data;
        uint32_t size;

    public:
        explicit circular_buffer(uint32_t cap)
            : data(std::make_unique<char[]>(cap)), size(cap) {}

        void write_at(uint32_t pos, const char *src, uint32_t len)
        {
            uint32_t idx = pos % size;
            if (idx + len <= size)
                std::memcpy(data.get() + idx, src, len);
            else
            {
                uint32_t first = size - idx;
                std::memcpy(data.get() + idx, src, first);
                std::memcpy(data.get(), src + first, len - first);
            }
        }

        void read_at(uint32_t pos, char *dst, uint32_t len)
        {
            uint32_t idx = pos % size;
            if (idx + len <= size)
                std::memcpy(dst, data.get() + idx, len);
            else
            {
                uint32_t first = size - idx;
                std::memcpy(dst, data.get() + idx, first);
                std::memcpy(dst + first, data.get(), len - first);
            }
        }

        uint32_t capacity() const { return size; }
    };

    /* ================= RECEIVE WINDOW ================= */

    class receive_window
    {
        circular_buffer buffer;
        uint32_t rcv_nxt = 0;
        uint32_t app_read = 0;
        bool ack_pending = false;

        uint32_t window_sz = 65535;

        std::map<uint32_t, std::vector<char>> ooo;

    public:
        explicit receive_window(uint32_t cap)
            : buffer(cap) {}

        bool receive_packet(const char *pkt, uint32_t sz, channel_header &h)
        {
            if (!packet_codec::deserialize_header(pkt, sz, h))
                return false;

            uint32_t payload_len = sz - channel_config::HEADER_SIZE;
            const char *payload = pkt + channel_config::HEADER_SIZE;

            if (payload_len == 0)
            {
                ack_pending = true;
                return true;
            }

            uint32_t start = h.seq_no;

            /* STRICT RULE: no partial overlap allowed */
            if (seq_ge(start, rcv_nxt))
            {
                // ignore out of order packets if its looping , for simplicity in logic

                uint32_t avail_space = channel_config::DEFAULT_BUFFER_SIZE - available();

                if (avail_space >= payload_len)
                {
                    if (start == rcv_nxt)
                    {
                        buffer.write_at(rcv_nxt, payload, payload_len);
                        rcv_nxt += payload_len;
                        advance_sack();
                    }

                    /* Out-of-order but valid */
                    if (!ooo.count(start))
                        ooo[start] = std::vector<char>(payload, payload + payload_len);
                }
            }

            ack_pending = true;
            return true;
        }

        uint32_t available() const { return rcv_nxt - app_read; }

        ssize_t read(char *dst, uint32_t len)
        {
            uint32_t avail = available();
            if (!avail)
                return 0;

            uint32_t n = std::min(avail, len);
            buffer.read_at(app_read, dst, n);
            app_read += n;
            return n;
        }

        uint32_t ack_no() const { return rcv_nxt; }

        uint16_t window() const
        {
            return static_cast<uint16_t>(std::min(channel_config::DEFAULT_BUFFER_SIZE - available(), uint32_t(0xFFFF)));
        }

        bool need_ack() const { return ack_pending; }
        void clear_ack() { ack_pending = false; }

    private:
        void advance_sack()
        {
            while (!ooo.empty())
            {
                auto it = ooo.begin();
                if (it->first != rcv_nxt)
                    break;

                buffer.write_at(rcv_nxt, it->second.data(), it->second.size());
                rcv_nxt += it->second.size();
                ooo.erase(it);
            }
        }
    };

    /* ================= SEND WINDOW ================= */

    struct inflight
    {
        uint32_t seq, len;
        uint64_t last;
        uint32_t retries;
    };

    class send_window
    {
        circular_buffer buffer;
        uint32_t snd_una = 0, snd_nxt = 0, write_pos = 0;
        uint16_t remote_win = 65535;
        std::deque<inflight> inflight_q;

        static uint64_t now_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

    public:
        explicit send_window(uint32_t cap) : buffer(cap) {}

        ssize_t write(const char *src, uint32_t len)
        {
            uint32_t buffered = write_pos - snd_una;
            uint32_t available = channel_config::DEFAULT_BUFFER_SIZE - buffered;

            if (available <= 0)
                return 0;

            uint32_t to_write = std::min(len, available);

            buffer.write_at(write_pos, src, to_write);
            write_pos += to_write;

            return static_cast<ssize_t>(to_write);
        }

        uint32_t max_payload() const
        {
            uint32_t avail = write_pos - snd_nxt;
            uint32_t inflight = snd_nxt - snd_una;
            if (!avail || inflight >= remote_win)
                return 0;

            return std::min({avail,
                             uint32_t(remote_win - inflight),
                             uint32_t(channel_config::MAX_MSS)});
        }

        uint32_t get_packet(char *dst, uint32_t cap, uint32_t &seq)
        {
            uint64_t now = now_ms();

            for (auto &seg : inflight_q)
            {
                if (now - seg.last > channel_config::RTO_MS &&
                    seg.retries < channel_config::MAX_RETRANSMITS)
                {
                    if (seg.len > cap)
                        return 0;

                    buffer.read_at(seg.seq, dst, seg.len);
                    seg.last = now;
                    seg.retries++;
                    seq = seg.seq;
                    return seg.len;
                }
            }

            uint32_t can = max_payload();
            if (!can || can > cap)
                return 0;

            buffer.read_at(snd_nxt, dst, can);
            inflight_q.push_back({snd_nxt, can, now, 0});
            seq = snd_nxt;
            snd_nxt += can;
            return can;
        }

        void ack(uint32_t ack_no)
        {
            if (seq_ge(ack_no, snd_una) && seq_lt(ack_no, snd_nxt))
            {
                snd_una = std::max(snd_una, ack_no);
                while (!inflight_q.empty() &&
                       inflight_q.front().seq + inflight_q.front().len <= snd_una)
                    inflight_q.pop_front();
            }
        }

        void update_window(uint16_t w) { remote_win = w; }
    };

    /* ================= CHANNEL ================= */

    class reliable_ordered_channel : public i_channel,
                                     public std::enable_shared_from_this<reliable_ordered_channel>
    {
        channel_id id;
        send_window snd;
        receive_window rcv;
        std::mutex mtx;

        std::function<void()> on_app;
        std::function<void(std::unique_ptr<rudp_protocol_packet>)> on_net;

    public:
        explicit reliable_ordered_channel(channel_id cid)
            : id(cid),
              snd(channel_config::DEFAULT_BUFFER_SIZE),
              rcv(channel_config::DEFAULT_BUFFER_SIZE) {}

        std::unique_ptr<i_channel> clone() const override { return nullptr; }

        void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
        {
            if (!pkt)
                return;

            std::unique_ptr<rudp_protocol_packet> out;
            bool notify = false;

            {
                std::lock_guard lk(mtx);

                channel_header h{};
                if (!rcv.receive_packet(pkt->get_const_buffer() +
                                            rudp_protocol_packet::CHANNEL_HEADER_OFFSET,
                                        pkt->get_length() -
                                            rudp_protocol_packet::CHANNEL_HEADER_OFFSET,
                                        h))
                    return;

                if (h.flags & uint16_t(channel_flags::ACK))
                    snd.ack(h.ack_no);
                if (h.flags & uint16_t(channel_flags::WIND_SZ))
                    snd.update_window(h.win_sz);

                notify = rcv.available() > 0;

                if (rcv.need_ack())
                {
                    out = send_nolock();
                    rcv.clear_ack();
                }
            }

            if (notify && on_app)
                on_app();
            if (out && on_net)
                on_net(std::move(out));
        }

        ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
        {
            std::lock_guard lk(mtx);
            return rcv.read(buf, len);
        }

        ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
        {
            std::unique_ptr<rudp_protocol_packet> out;
            ssize_t ret;

            {
                std::lock_guard lk(mtx);
                ret = snd.write(buf, len);
                if (ret > 0)
                    out = send_nolock();
            }

            if (out && on_net)
                on_net(std::move(out));
            return ret;
        }

        void set_on_app_data_ready(std::function<void()> f) override { on_app = f; }
        void set_on_net_data_ready(std::function<void(std::unique_ptr<rudp_protocol_packet>)> f) override { on_net = f; }
        void set_timer_service(std::shared_ptr<i_timer_service>) override {}

    private:
        std::unique_ptr<rudp_protocol_packet> send_nolock()
        {
            uint32_t cap = snd.max_payload();
            if (!cap && !rcv.need_ack())
                return nullptr;

            uint32_t size = rudp_protocol_packet::CHANNEL_HEADER_OFFSET +
                            channel_config::HEADER_SIZE + cap;

            auto pkt = std::make_unique<rudp_protocol_packet>(size);
            char *buf = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            char *payload = buf + channel_config::HEADER_SIZE;

            uint32_t seq = 0;
            uint32_t len = snd.get_packet(payload, cap, seq);

            pkt->set_length(rudp_protocol_packet::CHANNEL_HEADER_OFFSET +
                            channel_config::HEADER_SIZE + len);

            channel_header h{};
            h.seq_no = seq;
            h.ack_no = rcv.ack_no();
            h.win_sz = rcv.window();
            h.flags = uint16_t(channel_flags::ACK);

            packet_codec::serialize_header(buf, h);
            return pkt;
        }
    };
}
