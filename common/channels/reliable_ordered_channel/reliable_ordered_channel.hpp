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
    // ... (Sequence number comparison helpers remain the same) ...
    inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
    inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
    inline bool seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
    inline bool seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

    namespace channel_config
    {
        constexpr uint16_t HEADER_SIZE = 16;
        constexpr uint32_t DEFAULT_BUFFER_SIZE = 1024 * 1024;
        constexpr uint16_t MAX_MSS = 32 * 1024; // Large MSS as per your config
        constexpr uint64_t RTO_MS = 200;
        constexpr uint32_t MAX_RETRANSMITS = 15;

        // Total reserve: internal protocol headers + channel header
        constexpr size_t TOTAL_HEADER_RESERVE =
            rudp_protocol_packet::CHANNEL_HEADER_OFFSET + HEADER_SIZE;
    }

    enum class channel_flags : uint16_t
    {
        ACK = 1
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
            // Using memcpy to avoid strict aliasing issues, compiler optimizes this to mov
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

    /* ================= PACKET WRAPPER ================= */

    // Helper to track sequence numbers with packets
    struct packet_entry
    {
        uint32_t start_seq;
        std::shared_ptr<rudp_protocol_packet> pkt;

        uint32_t payload_len() const
        {
            return pkt->get_length() - channel_config::TOTAL_HEADER_RESERVE;
        }

        uint32_t end_seq() const
        {
            return start_seq + payload_len();
        }
    };

    /* ================= RECEIVE WINDOW ================= */

    struct receive_window
    {
        // Queue of in-order packets ready to be read by app
        std::deque<packet_entry> in_order_q;

        // Out of order storage
        std::map<uint32_t, std::shared_ptr<rudp_protocol_packet>> ooo;

        uint32_t rcv_nxt = 0;
        uint32_t app_read_offset = 0; // Bytes read from the front packet
        bool ack_pending = false;

        uint32_t window_cap; // Max bytes we want to hold

    public:
        explicit receive_window(uint32_t cap = channel_config::DEFAULT_BUFFER_SIZE)
            : window_cap(cap) {}

        bool receive_packet(const char *raw_buf, uint32_t total_sz, channel_header &h, std::shared_ptr<rudp_protocol_packet> original_pkt)
        {
            if (!packet_codec::deserialize_header(raw_buf, total_sz, h))
                return false;

            uint32_t payload_len = total_sz - channel_config::HEADER_SIZE;

            // Header-only packet (ACK or Probe)
            if (payload_len == 0)
            {
                if (!(h.flags & (uint8_t)channel_flags::ACK))
                    ack_pending = true;
                return true;
            }

            uint32_t start = h.seq_no;

            // Only accept if it's within valid window range
            if (seq_ge(start, rcv_nxt))
            {
                uint32_t buffer_usage = get_buffer_usage();
                uint32_t avail_space = (window_cap > buffer_usage) ? window_cap - buffer_usage : 0;

                if (avail_space >= payload_len)
                {
                    if (start == rcv_nxt)
                    {
                        // In-order: push to queue
                        in_order_q.push_back({start, original_pkt});
                        rcv_nxt += payload_len;
                        advance_sack();
                    }
                    else if (ooo.find(start) == ooo.end())
                    {
                        // Out-of-order: stash it
                        ooo[start] = original_pkt;
                    }
                }
            }

            ack_pending = true;
            return true;
        }

        ssize_t read(char *dst, uint32_t len)
        {
            uint32_t total_read = 0;
            auto prev_win = window();

            while (len > 0 && !in_order_q.empty())
            {
                packet_entry &front = in_order_q.front();
                uint32_t pkt_payload_len = front.payload_len();
                uint32_t available_in_pkt = pkt_payload_len - app_read_offset;

                uint32_t to_copy = std::min(len, available_in_pkt);

                // Direct copy from packet payload to user buffer
                char *pkt_payload_start = front.pkt->get_buffer() + channel_config::TOTAL_HEADER_RESERVE;
                std::memcpy(dst, pkt_payload_start + app_read_offset, to_copy);

                dst += to_copy;
                len -= to_copy;
                total_read += to_copy;
                app_read_offset += to_copy;

                // If packet fully consumed, remove it
                if (app_read_offset >= pkt_payload_len)
                {
                    in_order_q.pop_front();
                    app_read_offset = 0;
                }
            }

            auto new_win = window();
            if (prev_win < channel_config::MAX_MSS && new_win >= channel_config::MAX_MSS)
                ack_pending = true;

            return total_read;
        }

        uint32_t available() const
        {
            // Total bytes available to read across all in-order packets
            uint32_t sum = 0;
            for (const auto &entry : in_order_q)
            {
                sum += entry.payload_len();
            }
            return sum - app_read_offset;
        }

        uint32_t ack_no() const { return rcv_nxt; }

        uint32_t get_buffer_usage() const
        {
            // Roughly bytes stored in in_order + ooo
            return available() + get_ooo_size();
        }

        uint32_t get_ooo_size() const
        {
            uint32_t sum = 0;
            for (auto const &[seq, pkt] : ooo)
            {
                sum += (pkt->get_length() - channel_config::TOTAL_HEADER_RESERVE);
            }
            return sum;
        }

        uint32_t window() const
        {
            uint32_t usage = get_buffer_usage();
            return (window_cap > usage) ? window_cap - usage : 0;
        }

        bool need_ack() const { return ack_pending; }
        void clear_ack() { ack_pending = false; }

    private:
        void advance_sack()
        {
            while (!ooo.empty())
            {
                auto it = ooo.begin();
                uint32_t seq = it->first;

                if (seq != rcv_nxt)
                    break;

                std::shared_ptr<rudp_protocol_packet> pkt = it->second;
                ooo.erase(it);

                in_order_q.push_back({seq, pkt});
                rcv_nxt += (pkt->get_length() - channel_config::TOTAL_HEADER_RESERVE);
            }
        }
    };

    /* ================= SEND WINDOW ================= */

    struct send_window
    {
        // The queue of packets that form our window
        std::deque<packet_entry> packets;

        uint32_t snd_una = 0;   // Sequence of the first byte of the first packet
        uint32_t snd_nxt = 0;   // Sequence we have sent up to
        uint32_t write_seq = 0; // Sequence we have written up to (tail of queue)

        uint32_t remote_win = channel_config::DEFAULT_BUFFER_SIZE;
        uint32_t buffer_cap;

    public:
        explicit send_window(uint32_t cap = channel_config::DEFAULT_BUFFER_SIZE) : buffer_cap(cap) {}

        uint32_t write(const char *src, uint32_t len)
        {
            uint32_t buffered = write_seq - snd_una;
            if (buffered >= buffer_cap)
                return 0; // Local buffer full

            uint32_t written = 0;

            while (len > 0)
            {
                // Check if we can append to the last packet
                bool used_existing = false;
                if (!packets.empty())
                {
                    auto &last = packets.back();
                    size_t current_len = last.pkt->get_length();
                    size_t capacity = last.pkt->get_capacity();

                    // We can append if there is space AND it hasn't been sent yet.
                    // (Simplification: if snd_nxt > last.start_seq, we consider it "sealed"
                    // to avoid changing data being transmitted)
                    if (current_len < capacity && seq_lt(last.start_seq, snd_nxt) == false)
                    {
                        size_t space = capacity - current_len;
                        size_t chunk = std::min((size_t)len, space);

                        std::memcpy(last.pkt->get_buffer() + current_len, src, chunk);
                        last.pkt->set_length(current_len + chunk);

                        src += chunk;
                        len -= chunk;
                        written += chunk;
                        write_seq += chunk;
                        used_existing = true;
                    }
                }

                if (!used_existing && len > 0)
                {
                    // Create new packet
                    // Max capacity is MSS + Headers
                    size_t alloc_size = channel_config::MAX_MSS + channel_config::TOTAL_HEADER_RESERVE;
                    auto new_pkt = std::make_shared<rudp_protocol_packet>(alloc_size);

                    // Reserve header space initially
                    new_pkt->set_length(channel_config::TOTAL_HEADER_RESERVE);

                    packets.push_back({write_seq, new_pkt});
                    // Loop will continue and fill this packet in the next iteration block
                }

                if (buffered + written >= buffer_cap)
                    break;
            }

            return written;
        }

        uint32_t get_in_flight() const
        {
            return snd_nxt - snd_una;
        }

        // Returns a pointer to a packet that needs sending, and fills its header seq
        // Returns nullptr if nothing to send
        std::shared_ptr<rudp_protocol_packet> get_next_packet_to_send(uint32_t &out_seq)
        {
            uint32_t inflight = snd_nxt - snd_una;
            if (inflight >= remote_win)
                return nullptr;

            // Find the packet that contains snd_nxt
            for (auto &entry : packets)
            {
                // Check if this packet starts at or after snd_nxt
                // Or if snd_nxt is inside this packet (unlikely with full packet sends, but possible)
                if (seq_ge(snd_nxt, entry.start_seq) && seq_lt(snd_nxt, entry.end_seq()))
                {
                    // This is the packet to send
                    out_seq = entry.start_seq;

                    // Advance snd_nxt to the end of this packet
                    uint32_t payload_sz = entry.payload_len();

                    // Cap by remote window?
                    // For simplicity, we send full packets. RUDP usually sends full frames.
                    // If window is too small for a full packet, we pause (Nagle/Flow Control)
                    if (inflight + payload_sz > remote_win && inflight > 0)
                        return nullptr;

                    snd_nxt = entry.end_seq();
                    return entry.pkt;
                }
                else if (seq_gt(entry.start_seq, snd_nxt))
                {
                    // snd_nxt is somehow behind packets? Should not happen in linear buffer
                    // But if it does, this is the next one.
                    out_seq = entry.start_seq;
                    snd_nxt = entry.end_seq();
                    return entry.pkt;
                }
            }
            return nullptr;
        }

        // Peek at max payload we *could* send (for Nagle check)
        uint32_t max_payload_pending() const
        {
            if (packets.empty())
                return 0;
            uint32_t total_avail = write_seq - snd_nxt;
            uint32_t inflight = snd_nxt - snd_una;
            if (inflight >= remote_win)
                return 0;
            return std::min(total_avail, remote_win - inflight);
        }

        void ack(uint32_t ack_no)
        {
            if (seq_ge(ack_no, snd_una) && seq_le(ack_no, snd_nxt))
            {
                snd_una = ack_no;

                // Remove fully acknowledged packets from the front
                while (!packets.empty())
                {
                    // If the END of the packet is <= ack_no, it is fully acked
                    if (seq_le(packets.front().end_seq(), snd_una))
                    {
                        packets.pop_front();
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }

        void update_window(uint32_t w) { remote_win = w; }
        uint32_t get_window() const { return remote_win; }
    };

    /* ================= CHANNEL ================= */

    struct inflight
    {
        uint32_t seq, len;
        uint32_t retries;
        uint32_t ack_rcvd_count = 0; // sender sent an ack  to get this guy , so i will resend immeditalty if this is  == 3
        std::shared_ptr<rudp_protocol_packet> pkt;

        inflight(uint32_t sequ, uint32_t length, uint32_t retry, std::shared_ptr<rudp_protocol_packet> packet)
            : seq(sequ), len(length), retries(retry), pkt(packet) {}
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

        // ... (Timer helpers: add_probing_timer_helper, add_rto_timer_helper remain identical) ...

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
                            sp->to_probe = true;
                            sp->send_nolock();
                            sp->add_probing_timer_helper(cur_prober_weak);
                        }
                        else
                            sp = nullptr;
                    }
                }
            };
            if (auto sp = cur_prober_weak.lock())
                if (sp->retries < channel_config::MAX_RETRANSMITS)
                {
                    global_timer_manager->add_timer(std::make_shared<timer_info>(duration_ms(1000 * (1ll << sp->retries)), cb));
                    sp->retries++;
                }
        }

        void add_probing_timer()
        {
            my_prober = std::make_shared<prober>(0);
            add_probing_timer_helper(my_prober);
        }

        void add_rto_timer_helper(std::weak_ptr<inflight> nxt_weak)
        {
            std::weak_ptr this_weak = shared_from_this();
            auto cb = [this_weak, nxt_weak]
            {
                if (auto sp = this_weak.lock())
                    if (auto inflight_sp = nxt_weak.lock())
                    {
                        inflight_sp->retries++;
                        sp->on_net(inflight_sp->pkt);
                        sp->add_rto_timer_helper(nxt_weak);
                    }
            };
            if (auto sp = nxt_weak.lock())
                if (sp->retries < channel_config::MAX_RETRANSMITS)
                    global_timer_manager->add_timer(std::make_shared<timer_info>(duration_ms(channel_config::RTO_MS * (1ll << sp->retries)), cb));
        }

        void add_rto_timer(uint32_t seq_no, std::shared_ptr<rudp_protocol_packet> pkt)
        {
            long long len = pkt->get_length() - channel_config::TOTAL_HEADER_RESERVE;
            if (len <= 0)
                return;
            auto nxt = std::make_shared<inflight>(seq_no, len, 0, pkt);
            inflight_q.push_back(nxt);
            add_rto_timer_helper(nxt);
        }

    public:
        explicit reliable_ordered_channel(channel_id cid)
            : id(cid), snd(channel_config::DEFAULT_BUFFER_SIZE), rcv(channel_config::DEFAULT_BUFFER_SIZE) {}

        std::unique_ptr<i_channel> clone() const override { return nullptr; }

        void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt_unique) override
        {
            if (!pkt_unique)
                return;

            // Convert to shared_ptr because we might store it in ooo/in_order maps
            std::shared_ptr<rudp_protocol_packet> pkt = std::move(pkt_unique);
            bool notify = false;

            {
                std::lock_guard lk(mtx);
                channel_header h{};

                // Get pointer to channel header (skip session headers)
                const char *ch_header_ptr = pkt->get_const_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
                uint32_t ch_packet_len = pkt->get_length() - rudp_protocol_packet::CHANNEL_HEADER_OFFSET;

                if (!rcv.receive_packet(ch_header_ptr, ch_packet_len, h, pkt))
                    return;

                if (h.flags & uint16_t(channel_flags::ACK))
                {
                    for (auto it = inflight_q.begin(); it != inflight_q.end();)
                    {
                        if (seq_le((*it)->seq + (*it)->len, h.ack_no))
                            it = inflight_q.erase(it);
                        else if ((*it)->seq == h.ack_no)
                        {
                            if (((++((*it)->ack_rcvd_count)) % 3) == 0)
                            {
                                on_net((*it)->pkt);
                                (*it)->retries++;
                            }
                            break;
                        }
                        else
                            break;
                    }

                    snd.ack(h.ack_no);
                    uint32_t prev_win = snd.get_window();
                    snd.update_window(h.win_sz);
                    if (prev_win > 0 && snd.get_window() == 0)
                        add_probing_timer();
                }

                send_nolock();
                notify = rcv.available() > 0;
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
            send_nolock();
            return ret;
        }

        ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
        {
            std::lock_guard lk(mtx);
            ssize_t ret = snd.write(buf, len);
            send_nolock();
            return ret;
        }

        void set_on_app_data_ready(std::function<void()> f) override { on_app = f; }
        void set_on_net_data_ready(std::function<void(std::shared_ptr<rudp_protocol_packet>)> f) override { on_net = f; }
        void set_timer_service(std::shared_ptr<i_timer_service> timer_service) override { global_timer_manager = timer_service; }

    private:
        void send_nolock()
        {
            while (true)
            {
                // Nagle Check
                uint32_t pending_bytes = snd.max_payload_pending();
                uint32_t in_flight_cnt = snd.get_in_flight();

                if (!(rcv.need_ack() || to_probe ||
                      (pending_bytes > 0 && (in_flight_cnt == 0 || pending_bytes == channel_config::MAX_MSS))))
                    break;

                // If only ACKs are needed and no data, create a specific ACK packet
                if (pending_bytes == 0 && (rcv.need_ack() || to_probe))
                {
                    // Create pure ACK/Probe packet
                    auto pkt = std::make_shared<rudp_protocol_packet>(channel_config::TOTAL_HEADER_RESERVE);
                    pkt->set_length(channel_config::TOTAL_HEADER_RESERVE);

                    // Serialize header...
                    channel_header h{};
                    h.seq_no = snd.snd_nxt;
                    fill_ack_info(h);

                    if (to_probe)
                        to_probe = false;

                    char *buf = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
                    packet_codec::serialize_header(buf, h);
                    on_net(pkt);

                    // Pure ACKs are not retransmitted usually, or handled differently.
                    // Loop break to avoid infinite loop if logic expects data
                    break;
                }

                uint32_t seq = 0;
                // Get the existing packet pointer from the queue
                std::shared_ptr<rudp_protocol_packet> pkt = snd.get_next_packet_to_send(seq);

                if (!pkt)
                    break;

                // Write Header into the RESERVED space
                channel_header h{};
                h.seq_no = seq;
                fill_ack_info(h);
                if (to_probe)
                    to_probe = false;

                char *buf = pkt->get_buffer() + rudp_protocol_packet::CHANNEL_HEADER_OFFSET;
                packet_codec::serialize_header(buf, h);

                on_net(pkt);
                add_rto_timer(seq, pkt);
            }
        }

        void fill_ack_info(channel_header &h)
        {
            if (rcv.need_ack())
            {
                h.flags = uint16_t(channel_flags::ACK);
                h.ack_no = rcv.ack_no();
                uint32_t win_to_send = rcv.window();
                if (win_to_send < channel_config::MAX_MSS)
                    win_to_send = 0; // Clark's
                h.win_sz = win_to_send;
                rcv.clear_ack();
            }
        }
    };
}