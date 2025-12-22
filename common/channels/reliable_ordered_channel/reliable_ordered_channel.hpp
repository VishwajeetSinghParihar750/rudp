#pragma once

#include "../../i_channel.hpp"
#include "../../types.hpp"
#include "../../rudp_protocol_packet.hpp"
#include "../../i_timer_service.hpp"
#include "../../timer_info.hpp"
#include "../../logger.hpp"

#include <cassert>
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
        constexpr uint16_t HEADER_SIZE = 16;
        constexpr uint32_t DEFAULT_BUFFER_SIZE = 1024 * 1024;
        constexpr uint16_t MAX_MSS = 32 * 1024;
        constexpr uint64_t RTO_MS = 400;
        constexpr uint32_t MAX_RETRANSMITS = 15;
    }

    enum class channel_flags : uint16_t
    {
        ACK = 1,
    };

    struct channel_header
    {
        uint32_t seq_no;
        uint32_t ack_no;
        uint32_t win_sz;
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
            h.win_sz = ntohl(win);
            h.flags = ntohs(flg);
            h.checksum = ntohs(csum);
            return true;
        }

        static void serialize_header(char *buf, const channel_header &h)
        {
            uint32_t seq = htonl(h.seq_no);
            uint32_t ack = htonl(h.ack_no);
            uint32_t win = htonl(h.win_sz);
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
        explicit circular_buffer(uint32_t cap = channel_config::DEFAULT_BUFFER_SIZE)
            : data(std::make_unique<char[]>(cap)), size(cap) {}

        void write_at(uint32_t pos, const char *src, uint32_t len)
        {
            assert(len <= size);

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
            assert(len <= size);

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

    struct receive_window
    {
        circular_buffer buffer;
        uint32_t rcv_nxt = 0;
        uint32_t app_read = 0;
        bool ack_pending = false;

        uint32_t window_sz = 0;

        std::map<uint32_t, uint32_t> ooo; // keeps start and len

    public:
        explicit receive_window(uint32_t cap = channel_config::DEFAULT_BUFFER_SIZE)
            : buffer(cap), window_sz(cap) {}

        bool receive_packet(const char *pkt, uint32_t sz, channel_header &h) // returns if it kept any bytes
        {
            if (!packet_codec::deserialize_header(pkt, sz, h))
                return false;

            uint32_t payload_len = sz - channel_config::HEADER_SIZE;
            const char *payload = pkt + channel_config::HEADER_SIZE;

            if (payload_len == 0)
            {
                if (!(h.flags & (uint8_t)channel_flags::ACK))
                {
                    ack_pending = true; // means a prober for window sz
                    // std::cout << "got a probe message " << std::endl;
                }
                return true;
            }

            uint32_t start = h.seq_no;

            if (seq_ge(start, rcv_nxt))
            {
                uint32_t avail_space = window_sz - available();

                if (avail_space >= payload_len)
                {
                    buffer.write_at(rcv_nxt, payload, payload_len);

                    if (start == rcv_nxt)
                    {
                        rcv_nxt += payload_len;
                        advance_sack();
                    }
                    else if (!ooo.count(start))
                        ooo[start] = payload_len;
                }
            }

            ack_pending = true;
            return true;
        }

        uint32_t available() const { return rcv_nxt - app_read; }

        ssize_t read(char *dst, uint32_t len)
        {
            uint32_t avail = available();
            uint32_t n = std::min(avail, len);

            if (n == 0)
                return 0;
            buffer.read_at(app_read, dst, n);
            app_read += n;
            return n;
        }

        uint32_t ack_no() const { return rcv_nxt; }

        uint32_t window() const
        {
            return window_sz;
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
                rcv_nxt += it->second;
                ooo.erase(it);
            }
        }
    };

    /* ================= SEND WINDOW ================= */

    struct send_window
    {
        circular_buffer buffer;
        uint32_t snd_una = 0, snd_nxt = 0, write_pos = 0;
        uint32_t remote_win = channel_config::DEFAULT_BUFFER_SIZE;

    public:
        explicit send_window(uint32_t cap = channel_config::DEFAULT_BUFFER_SIZE) : buffer(cap) {}

        uint32_t write(const char *src, uint32_t len)
        {
            uint32_t buffered = write_pos - snd_una;
            uint32_t available = buffer.capacity() - buffered;

            uint32_t to_write = std::min(len, available);

            if (to_write == 0)
                return 0;

            buffer.write_at(write_pos, src, to_write);
            write_pos += to_write;

            return to_write;
        }
        //
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

        uint32_t get_packet(char *dst, uint32_t cap, uint32_t &seq) // this wil always mean a new packet , not retransmission
        {

            seq = snd_nxt;

            uint32_t can = max_payload();
            if (can == 0 or cap == 0)
                return 0;

            buffer.read_at(snd_nxt, dst, can);
            snd_nxt += can;
            return can;
        }

        void ack(uint32_t ack_no)
        {
            if (seq_ge(ack_no, snd_una) && seq_le(ack_no, snd_nxt))
            {
                snd_una = ack_no;
            }
        }

        void update_window(uint32_t w)
        {
            remote_win = w;
        }

        uint32_t get_window() const
        {
            return remote_win;
        }
    };

    /* ================= CHANNEL ================= */

    struct inflight
    {
        uint32_t seq, len;
        uint32_t retries;
        std::shared_ptr<rudp_protocol_packet> pkt;

        inflight(uint32_t sequ, uint32_t length, uint32_t retry, std::shared_ptr<rudp_protocol_packet> packet) : seq(sequ), len(length), retries(retry), pkt(packet) {}
    };

    struct prober
    {
        uint32_t retries;
        prober(uint32_t retry) : retries(0) {}
    };

    class reliable_ordered_channel : public i_channel,
                                     public std::enable_shared_from_this<reliable_ordered_channel>
    {
        channel_id id;
        send_window snd;

        std::deque<std::shared_ptr<inflight>> inflight_q;

        receive_window rcv;
        std::mutex mtx;

        std::function<void()> on_app;
        std::function<void(std::shared_ptr<rudp_protocol_packet>)> on_net;

        std::shared_ptr<i_timer_service> global_timer_manager;

        std::shared_ptr<prober> my_prober = nullptr;
        bool to_probe = false;

        void add_probing_timer_helper(std::weak_ptr<prober> cur_prober_weak)
        {
            std::weak_ptr this_weak = shared_from_this();

            auto cb = [this_weak, cur_prober_weak]
            {
                if (auto prober_sp = cur_prober_weak.lock())
                {
                    if (auto sp = this_weak.lock())
                    {
                        std::unique_lock lock(sp->mtx);

                        if (sp->snd.get_window() == 0)
                        {
                            std::vector<std::pair<uint32_t, std::shared_ptr<rudp_protocol_packet>>> out;

                            sp->to_probe = true; // set to probe

                            sp->send_nolock(out);
                            for (auto &i : out)
                            {
                                sp->on_net(i.second);
                                sp->add_rto_timer(i.first, i.second);
                            }
                            sp->add_probing_timer_helper(cur_prober_weak);
                        }
                        else
                            sp = nullptr; // prober gone
                    }
                }
            };

            if (auto sp = cur_prober_weak.lock())
                if (sp->retries < channel_config::MAX_RETRANSMITS)
                {
                    global_timer_manager->add_timer(std::make_shared<timer_info>(duration_ms(1000 * (1ll << sp->retries)), cb));
                    sp->retries++;
                }
                else
                    sp = nullptr;
        }

        void add_probing_timer()
        {
            my_prober = std::make_shared<prober>(0); // new prober,old gone
            add_probing_timer_helper(my_prober);
        }

        void add_rto_timer_helper(std::weak_ptr<inflight> nxt_weak)
        {
            std::weak_ptr this_weak = shared_from_this();

            auto cb = [this_weak, nxt_weak]
            {
                if (auto sp = this_weak.lock())
                {
                    if (auto inflight_sp = nxt_weak.lock())
                    {

                        inflight_sp->retries++;
                        sp->on_net(inflight_sp->pkt);
                        sp->add_rto_timer_helper(nxt_weak);
                    }
                }
            };

            if (auto sp = nxt_weak.lock())
                if (sp->retries < channel_config::MAX_RETRANSMITS)
                    global_timer_manager->add_timer(std::make_shared<timer_info>(duration_ms(channel_config::RTO_MS * (1ll << sp->retries)), cb));
        }

        void add_rto_timer(uint32_t seq_no, std::shared_ptr<rudp_protocol_packet> pkt)
        {

            long long len = pkt->get_length() - channel_config::HEADER_SIZE - rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            if (len <= 0)
                return;

            auto nxt = std::make_shared<inflight>(seq_no, len, 0, pkt);
            inflight_q.push_back(nxt);
            add_rto_timer_helper(nxt);
        }

        // std::jthread debug_thread;

    public:
        explicit reliable_ordered_channel(channel_id cid)
            : id(cid),
              snd(channel_config::DEFAULT_BUFFER_SIZE),
              rcv(channel_config::DEFAULT_BUFFER_SIZE)
        {
            // debug_thread = std::jthread([this](std::stop_token stoken)
            //                             {

            // while (!stoken.stop_requested()) {
            //     {
            //         std::cout << "waiting lock debug thread " << std::endl;
            //         std::unique_lock lk(mtx);
            //         std::cout << "came debug thread " << std::endl;
            //             std::cout
            //                 << "[DBG][CH " << id << "] "
            //                 << "snd_una=" << snd.snd_una
            //                 << " snd_nxt=" << snd.snd_nxt
            //                 << " snd_wnd=" << snd.get_window()
            //                 << " rcv_avail=" << rcv.available()
            //                 << " rcv_wnd=" << rcv.window()
            //                 << " inflight=" << inflight_q.size()
            //                 << " to_probe=" << to_probe
            //                 << " probe_active=" << (my_prober != nullptr)
            //                 << std::endl;

            //     }

            //     std::this_thread::sleep_for(duration_ms(1000));
            // } });
        }

        std::unique_ptr<i_channel> clone() const override { return nullptr; }

        void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
        {
            if (!pkt)
                return;

            std::vector<std::pair<uint32_t, std::shared_ptr<rudp_protocol_packet>>> out;
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

                bool probe = false;

                if (h.flags & uint16_t(channel_flags::ACK))
                {
                    for (auto it = inflight_q.begin(); it != inflight_q.end();)
                        if (seq_le((*it)->seq + (*it)->len, h.ack_no))
                            it = inflight_q.erase(it);
                        else
                            break;

                    snd.ack(h.ack_no);
                    uint32_t prev_win = snd.get_window();
                    snd.update_window(h.win_sz);

                    uint32_t new_window = snd.get_window();

                    if (prev_win > 0 && new_window == 0)
                    {
                        add_probing_timer();
                    }
                }

                notify = rcv.available() > 0;

                send_nolock(out);

                if (!out.empty() && on_net)
                    for (auto &i : out)
                    {
                        on_net(i.second);
                        add_rto_timer(i.first, i.second);
                    }
            }

            if (notify && on_app)
                on_app();
        }

        ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
        {
            std::lock_guard lk(mtx);
            ssize_t ret = rcv.read(buf, len);

            if (rcv.available() > 0)
                on_app();

            return ret;
        }

        ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
        {
            std::vector<std::pair<uint32_t, std::shared_ptr<rudp_protocol_packet>>> out;
            ssize_t ret = 0;

            {
                std::lock_guard lk(mtx);
                ret = snd.write(buf, len);

                if (ret > 0)
                    send_nolock(out);

                if (!out.empty() && on_net)
                    for (auto &i : out)
                    {
                        on_net(i.second);
                        add_rto_timer(i.first, i.second);
                    }
            }

            return ret;
        }

        void
        set_on_app_data_ready(std::function<void()> f) override
        {
            on_app = f;
        }
        void set_on_net_data_ready(std::function<void(std::shared_ptr<rudp_protocol_packet>)> f) override { on_net = f; }
        void set_timer_service(std::shared_ptr<i_timer_service> timer_service) override { global_timer_manager = timer_service; }

    private:
        void send_nolock(std::vector<std::pair<uint32_t, std::shared_ptr<rudp_protocol_packet>>> &out)
        {
            uint32_t cap = snd.max_payload();
            if (!cap && !rcv.need_ack() && !to_probe)
                return;

            uint32_t size = rudp_protocol_packet::CHANNEL_HEADER_OFFSET +
                            channel_config::HEADER_SIZE + cap;

            auto pkt = std::make_shared<rudp_protocol_packet>(size);
            char *buf = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
            char *payload = buf + channel_config::HEADER_SIZE;

            uint32_t seq = 0;
            uint32_t len = snd.get_packet(payload, cap, seq);

            pkt->set_length(rudp_protocol_packet::CHANNEL_HEADER_OFFSET +
                            channel_config::HEADER_SIZE + len);

            channel_header h{};
            h.seq_no = seq;

            if (rcv.need_ack())
            {
                h.flags = uint16_t(channel_flags::ACK);
                h.ack_no = rcv.ack_no();
                h.win_sz = rcv.window();
                rcv.clear_ack();
            }
            else if (to_probe) // coz ack and probe cant go together
            {
                to_probe = false;
            }

            packet_codec::serialize_header(buf, h);

            out.push_back({seq, pkt});

            send_nolock(out);
        }
    };
}
