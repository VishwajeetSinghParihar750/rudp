#pragma once

#include "../../i_channel.hpp"
#include "../../rudp_protocol_packet.hpp"
#include "../../rudp_protocol.hpp"
#include "../../types.hpp"
#include "../../timer_info.hpp"
#include "../../timer_manager.hpp"

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

namespace ordered_channel_config
{
    constexpr uint16_t HEADER_SIZE = 14;
    constexpr uint32_t DEFAULT_BUFFER_SIZE = 64 * 1024;
    constexpr uint16_t MAX_MSS = 1400;
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

inline uint64_t get_current_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline uint32_t fold_checksum(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return sum;
}

class packet_codec
{
public:
    static bool deserialize_header(const char *buf, uint32_t total_size, channel_header &out_header)
    {
        if (total_size < ordered_channel_config::HEADER_SIZE)
            return false;

        out_header.seq_no = ntohl(*reinterpret_cast<const uint32_t *>(buf + 0));
        out_header.ack_no = ntohl(*reinterpret_cast<const uint32_t *>(buf + 4));
        out_header.win_sz = ntohs(*reinterpret_cast<const uint16_t *>(buf + 8));
        out_header.flags = ntohs(*reinterpret_cast<const uint16_t *>(buf + 10));
        out_header.checksum = ntohs(*reinterpret_cast<const uint16_t *>(buf + 12));

        return verify_checksum(buf, total_size, out_header.checksum);
    }

    static void serialize_header(char *buf, const channel_header &header)
    {
        uint32_t seq = htonl(header.seq_no);
        uint32_t ack = htonl(header.ack_no);
        uint16_t win = htons(header.win_sz);
        uint16_t flg = htons(header.flags);
        uint16_t zero = 0;

        memcpy(buf + 0, &seq, 4);
        memcpy(buf + 4, &ack, 4);
        memcpy(buf + 8, &win, 2);
        memcpy(buf + 10, &flg, 2);
        memcpy(buf + 12, &zero, 2);
    }

    static uint16_t calculate_checksum(const char *buf, uint32_t len)
    {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < len; i += 2)
        {
            uint16_t val = (static_cast<uint8_t>(buf[i]) << 8);
            if (i + 1 < len)
                val |= static_cast<uint8_t>(buf[i + 1]);
            sum += val;
        }
        return static_cast<uint16_t>(fold_checksum(sum));
    }

private:
    static bool verify_checksum(const char *buf, uint32_t total_size, uint16_t stored_checksum)
    {
        uint32_t sum = 0;

        for (uint32_t i = 0; i < ordered_channel_config::HEADER_SIZE; i += 2)
        {
            if (i == 12)
                continue;
            uint16_t val = (static_cast<uint8_t>(buf[i]) << 8) | static_cast<uint8_t>(buf[i + 1]);
            sum += val;
        }

        const char *payload = buf + ordered_channel_config::HEADER_SIZE;
        uint32_t payload_len = total_size - ordered_channel_config::HEADER_SIZE;
        for (uint32_t i = 0; i < payload_len; ++i)
        {
            uint16_t val = static_cast<uint8_t>(payload[i]);
            sum += (i % 2 == 0) ? (val << 8) : val;
        }

        sum = fold_checksum(sum);
        uint16_t computed_checksum = static_cast<uint16_t>(~sum);
        return computed_checksum == stored_checksum;
    }
};

class circular_buffer
{
private:
    std::unique_ptr<char[]> data;
    uint32_t size;

public:
    explicit circular_buffer(uint32_t capacity = ordered_channel_config::DEFAULT_BUFFER_SIZE)
        : data(std::make_unique<char[]>(capacity)), size(capacity)
    {
    }

    void write_at_position(uint32_t position, const char *src, uint32_t len)
    {
        uint32_t idx = position % size;
        if (idx + len <= size)
        {
            memcpy(data.get() + idx, src, len);
        }
        else
        {
            uint32_t first_chunk = size - idx;
            memcpy(data.get() + idx, src, first_chunk);
            memcpy(data.get(), src + first_chunk, len - first_chunk);
        }
    }

    void read_from_position(uint32_t position, char *dst, uint32_t len)
    {
        uint32_t idx = position % size;
        if (idx + len <= size)
        {
            memcpy(dst, data.get() + idx, len);
        }
        else
        {
            uint32_t first_chunk = size - idx;
            memcpy(dst, data.get() + idx, first_chunk);
            memcpy(dst + first_chunk, data.get(), len - first_chunk);
        }
    }

    uint32_t capacity() const { return size; }
};

class sequence_tracker
{
private:
    std::atomic<uint32_t> seq_no{0};

public:
    uint32_t get() const { return seq_no.load(std::memory_order_acquire); }
    void set(uint32_t value) { seq_no.store(value, std::memory_order_release); }
    uint32_t advance(uint32_t delta)
    {
        return seq_no.fetch_add(delta, std::memory_order_release);
    }
};

class flow_controller
{
private:
    std::atomic<uint16_t> remote_window{65535};
    std::mutex lock;

public:
    uint16_t get_remote_window() const
    {
        return remote_window.load(std::memory_order_acquire);
    }

    void update_remote_window(uint16_t window_size)
    {
        remote_window.store(window_size, std::memory_order_release);
    }

    bool can_send_bytes(uint32_t in_flight) const
    {
        return in_flight < get_remote_window();
    }
};

struct inflight_segment
{
    uint32_t seq_no;
    uint32_t length;
    uint64_t last_sent_time;
    uint32_t retransmit_count;
};

class retransmission_manager
{
private:
    std::deque<inflight_segment> segments;
    std::mutex lock;

public:
    void add_segment(uint32_t seq_no, uint32_t length)
    {
        std::lock_guard<std::mutex> guard(lock);
        segments.push_back({seq_no, length, get_current_time_ms(), 0});
    }

    bool get_retransmit_segment(inflight_segment &out_seg)
    {
        std::lock_guard<std::mutex> guard(lock);
        uint64_t now = get_current_time_ms();

        for (auto &seg : segments)
        {
            if ((now - seg.last_sent_time) > ordered_channel_config::RTO_MS &&
                seg.retransmit_count < ordered_channel_config::MAX_RETRANSMITS)
            {
                seg.last_sent_time = now;
                seg.retransmit_count++;
                out_seg = seg;
                return true;
            }
        }
        return false;
    }

    void acknowledge_up_to(uint32_t ack_no)
    {
        std::lock_guard<std::mutex> guard(lock);
        while (!segments.empty() &&
               segments.front().seq_no + segments.front().length <= ack_no)
        {
            segments.pop_front();
        }
    }
};

class receive_window
{
private:
    circular_buffer buffer;
    sequence_tracker rcv_nxt;
    sequence_tracker app_read_seq;
    std::atomic<bool> ack_pending{false};
    std::map<uint32_t, std::vector<char>> out_of_order_segments;
    std::mutex lock;

public:
    explicit receive_window(uint32_t size = ordered_channel_config::DEFAULT_BUFFER_SIZE)
        : buffer(size)
    {
    }

    bool receive_packet(const char *packet, uint32_t packet_size, channel_header &out_header)
    {
        if (!packet_codec::deserialize_header(packet, packet_size, out_header))
            return false;

        std::lock_guard<std::mutex> guard(lock);

        uint32_t payload_len = packet_size - ordered_channel_config::HEADER_SIZE;
        if (payload_len == 0)
            return true;

        const char *payload = packet + ordered_channel_config::HEADER_SIZE;
        uint32_t seg_start = out_header.seq_no;
        uint32_t seg_end = out_header.seq_no + payload_len;
        uint32_t current_rcv_nxt = rcv_nxt.get();

        if (seg_end > app_read_seq.get() + buffer.capacity())
            return false;

        if (seg_start >= current_rcv_nxt)
        {
            // Out-of-order segment
            out_of_order_segments[seg_start] = std::vector<char>(payload, payload + payload_len);
            ack_pending.store(true, std::memory_order_release);
        }
        else if (seg_end > current_rcv_nxt)
        {
            // Partial overlap
            uint32_t overlap = current_rcv_nxt - seg_start;
            uint32_t new_data_len = seg_end - current_rcv_nxt;
            buffer.write_at_position(current_rcv_nxt, payload + overlap, new_data_len);
            rcv_nxt.set(seg_end);
            advance_with_sack();
            ack_pending.store(true, std::memory_order_release);
        }
        else
        {
            ack_pending.store(true, std::memory_order_release);
        }

        return true;
    }

    ssize_t read_data(char *out_buf, uint32_t len)
    {
        std::lock_guard<std::mutex> guard(lock);
        uint32_t current_rcv = rcv_nxt.get();
        uint32_t current_app_read = app_read_seq.get();
        uint32_t available = current_rcv - current_app_read;

        if (available == 0)
            return 0;

        uint32_t to_read = std::min(available, len);
        buffer.read_from_position(current_app_read, out_buf, to_read);
        app_read_seq.advance(to_read);

        return static_cast<ssize_t>(to_read);
    }

    uint32_t get_ack_no() const { return rcv_nxt.get(); }

    uint16_t get_window_size() const
    {
        uint32_t used = rcv_nxt.get() - app_read_seq.get();
        uint32_t available = (used < buffer.capacity()) ? (buffer.capacity() - used) : 0;
        return std::min(available, static_cast<uint32_t>(0xFFFF));
    }

    bool is_ack_pending() const { return ack_pending.load(std::memory_order_acquire); }
    void clear_ack_pending() { ack_pending.store(false, std::memory_order_release); }
    uint32_t get_initial_seq_no() const { return rcv_nxt.get(); }
    void set_initial_seq_no(uint32_t seq) { rcv_nxt.set(seq); }

private:
    void advance_with_sack()
    {
        uint32_t current_rcv = rcv_nxt.get();
        while (!out_of_order_segments.empty())
        {
            auto it = out_of_order_segments.begin();
            if (it->first == current_rcv)
            {
                buffer.write_at_position(current_rcv, it->second.data(), it->second.size());
                current_rcv += it->second.size();
                out_of_order_segments.erase(it);
            }
            else
            {
                break;
            }
        }
        rcv_nxt.set(current_rcv);
    }
};

class send_window
{
private:
    circular_buffer buffer;
    sequence_tracker snd_una;
    sequence_tracker snd_nxt;
    sequence_tracker write_pos;
    flow_controller flow;
    retransmission_manager retransmissions;

public:
    explicit send_window(uint32_t size = ordered_channel_config::DEFAULT_BUFFER_SIZE)
        : buffer(size)
    {
    }

    ssize_t write_data(const char *data, uint32_t len)
    {
        uint32_t current_write = write_pos.get();
        uint32_t current_una = snd_una.get();
        uint32_t buffered = (current_write - current_una + buffer.capacity()) % buffer.capacity();
        uint32_t available = (buffer.capacity() - 1) - buffered;

        if (available < len)
            return -1;

        buffer.write_at_position(current_write, data, len);
        write_pos.advance(len);

        return static_cast<ssize_t>(len);
    }

    uint32_t get_packet_to_send(char *out_buf, uint32_t max_payload, uint32_t &out_seq_no)
    {
        // Check for retransmission
        inflight_segment seg;
        if (retransmissions.get_retransmit_segment(seg))
        {
            buffer.read_from_position(seg.seq_no, out_buf, seg.length);
            out_seq_no = seg.seq_no;
            return seg.length;
        }

        // Send new data
        uint32_t current_nxt = snd_nxt.get();
        uint32_t current_write = write_pos.get();
        uint32_t available = (current_write - current_nxt + buffer.capacity()) % buffer.capacity();

        if (available == 0)
            return 0;

        uint32_t in_flight = current_nxt - snd_una.get();
        if (!flow.can_send_bytes(in_flight))
            return 0;

        uint32_t can_send = std::min({available, (uint32_t)ordered_channel_config::MAX_MSS,
                                      static_cast<uint32_t>(flow.get_remote_window() - in_flight)});

        if (can_send == 0)
            return 0;

        buffer.read_from_position(current_nxt, out_buf, can_send);
        retransmissions.add_segment(current_nxt, can_send);
        out_seq_no = current_nxt;
        snd_nxt.advance(can_send);

        return can_send;
    }

    void process_ack(uint32_t ack_no)
    {
        if (ack_no > snd_una.get())
        {
            snd_una.set(ack_no);
            retransmissions.acknowledge_up_to(ack_no);
        }
    }

    void process_window_update(uint16_t window_size)
    {
        flow.update_remote_window(window_size);
    }

    uint32_t get_initial_seq_no() const { return snd_nxt.get(); }
};

class flag_manager
{
private:
    std::atomic<bool> should_send_ack{false};
    std::atomic<bool> should_send_window{false};

public:
    void set_send_ack(bool value) { should_send_ack.store(value, std::memory_order_release); }
    void set_send_window(bool value) { should_send_window.store(value, std::memory_order_release); }

    bool get_send_ack() const { return should_send_ack.load(std::memory_order_acquire); }
    bool get_send_window() const { return should_send_window.load(std::memory_order_acquire); }
};

class reliable_ordered_channel : public i_channel
{
private:
    channel_id ch_id;
    send_window snd_window;
    receive_window rcv_window;
    flag_manager flags;
    std::function<void()> on_app_data_ready;
    std::function<void(std::unique_ptr<rudp_protocol_packet>)> on_net_data_ready;

    std::shared_ptr<timer_manager> global_timer_manager;

    // === Timer Configuration ===
    static constexpr uint64_t KEEPALIVE_INTERVAL_MS = 30000; // 30 seconds
    static constexpr uint64_t DELAYED_ACK_MS = 40;           // 40ms delayed ACK

    // === Timer Handlers ===
    void on_rto_expire()
    {
        // Retransmission timeout: trigger immediate send
        auto pkt = on_transport_send();
        if (pkt && on_net_data_ready)
            on_net_data_ready(std::move(pkt));

        // Reschedule RTO
        schedule_rto_timer();
    }

    void on_keepalive_expire()
    {
        // Send keepalive ACK if no data in flight
        flags.set_send_ack(true);
        auto pkt = on_transport_send();
        if (pkt && on_net_data_ready)
            on_net_data_ready(std::move(pkt));

        // Reschedule keepalive
        schedule_keepalive_timer();
    }

    void on_delayed_ack_expire()
    {
        // Flush pending ACK
        if (rcv_window.is_ack_pending())
        {
            auto pkt = on_transport_send();
            if (pkt && on_net_data_ready)
                on_net_data_ready(std::move(pkt));
        }
    }

    // === Timer Scheduling ===
    void schedule_rto_timer()
    {
        if (!global_timer_manager)
            return;

        auto cb = [this]()
        { this->on_rto_expire(); };
        auto timer = std::make_unique<timer_info>(duration_ms(ordered_channel_config::RTO_MS), cb);
        global_timer_manager->add_timer(std::move(timer));
    }

    void schedule_keepalive_timer()
    {
        if (!global_timer_manager)
            return;

        auto cb = [this]()
        { this->on_keepalive_expire(); };
        auto timer = std::make_unique<timer_info>(duration_ms(KEEPALIVE_INTERVAL_MS), cb);
        global_timer_manager->add_timer(std::move(timer));
    }

    void schedule_delayed_ack_timer()
    {
        if (!global_timer_manager)
            return;

        auto cb = [this]()
        { this->on_delayed_ack_expire(); };
        auto timer = std::make_unique<timer_info>(duration_ms(DELAYED_ACK_MS), cb);
        global_timer_manager->add_timer(std::move(timer));
    }

public:
    explicit reliable_ordered_channel(channel_id id) : ch_id(id) {}

    std::unique_ptr<i_channel> clone() const override
    {
        // Cannot clone: contains non-copyable members (atomics, mutexes)
        // Return null to indicate cloning is not supported
        return nullptr;
    }

    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (!pkt)
            return;

        const size_t off = rudp_protocol::CHANNEL_HEADER_OFFSET;
        const char *buf = pkt->get_const_buffer();
        size_t len = pkt->get_length();

        if (len <= off)
            return; // nothing for channel layer

        const char *ibuf = buf + off;
        uint32_t sz = static_cast<uint32_t>(len - off);

        channel_header header{};
        if (!rcv_window.receive_packet(ibuf, sz, header))
            return;

        if ((header.flags & static_cast<uint16_t>(channel_flags::ACK)) != 0)
            snd_window.process_ack(header.ack_no);

        if ((header.flags & static_cast<uint16_t>(channel_flags::WIND_SZ)) != 0)
            snd_window.process_window_update(header.win_sz);

        uint32_t payload_len = (sz > ordered_channel_config::HEADER_SIZE) ? (sz - ordered_channel_config::HEADER_SIZE) : 0;
        if (payload_len > 0 && on_app_data_ready)
            on_app_data_ready();

        // Schedule delayed ACK on data receipt
        if (payload_len > 0 && rcv_window.is_ack_pending())
            schedule_delayed_ack_timer();
        else if (rcv_window.is_ack_pending())
        {
            auto pkt_to_send = on_transport_send();
            if (pkt_to_send && on_net_data_ready)
                on_net_data_ready(std::move(pkt_to_send));
        }
    }

    std::unique_ptr<rudp_protocol_packet> on_transport_send() override
    {
        uint32_t seq_no = 0;
        char payload_buf[ordered_channel_config::MAX_MSS];
        uint32_t payload_len = snd_window.get_packet_to_send(payload_buf,
                                                             ordered_channel_config::MAX_MSS,
                                                             seq_no);

        bool send_ack = flags.get_send_ack() || rcv_window.is_ack_pending();
        bool send_window = flags.get_send_window();

        if (payload_len == 0 && !send_ack && !send_window)
            return nullptr;

        uint32_t total_size = rudp_protocol::CHANNEL_HEADER_OFFSET +
                              ordered_channel_config::HEADER_SIZE + payload_len;
        auto packet = std::make_unique<rudp_protocol_packet>(total_size);
        char *pkt_buf = packet->get_buffer() + rudp_protocol::CHANNEL_HEADER_OFFSET;

        channel_header header{};
        header.seq_no = seq_no;
        header.ack_no = rcv_window.get_ack_no();
        header.win_sz = rcv_window.get_window_size();
        header.flags = 0;

        if (send_ack)
            header.flags |= static_cast<uint16_t>(channel_flags::ACK);
        if (send_window)
            header.flags |= static_cast<uint16_t>(channel_flags::WIND_SZ);

        packet_codec::serialize_header(pkt_buf, header);

        if (payload_len > 0)
            memcpy(pkt_buf + ordered_channel_config::HEADER_SIZE, payload_buf, payload_len);

        uint16_t csum = packet_codec::calculate_checksum(pkt_buf,
                                                         ordered_channel_config::HEADER_SIZE + payload_len);
        uint16_t final_csum = htons(~csum);
        memcpy(pkt_buf + 12, &final_csum, 2);

        packet->set_length(total_size);
        rcv_window.clear_ack_pending();

        return packet;
    }

    ssize_t read_bytes_to_application(char *buf, const uint32_t &len) override
    {
        return rcv_window.read_data(buf, len);
    }

    ssize_t write_bytes_from_application(const char *buf, const uint32_t &len) override
    {
        ssize_t written = snd_window.write_data(buf, len);
        if (written > 0)
        {
            auto pkt = on_transport_send();
            if (pkt && on_net_data_ready)
                on_net_data_ready(std::move(pkt));
        }
        return written;
    }

    void set_on_app_data_ready(std::function<void()> f) override
    {
        on_app_data_ready = f;
    }

    void set_on_net_data_ready(std::function<void(std::unique_ptr<rudp_protocol_packet>)> f) override
    {
        on_net_data_ready = f;
    }

    void set_timer_manager(std::shared_ptr<timer_manager> timer_man) override
    {
        global_timer_manager = timer_man;
    }
};
