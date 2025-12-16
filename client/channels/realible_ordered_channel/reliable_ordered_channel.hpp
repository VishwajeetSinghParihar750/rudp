#pragma once

#include "../../i_channel.hpp"
#include "../../../common/rudp_protocol_packet.hpp"
#include "../../rudp_protocol.hpp"
#include "../../types.hpp"
#include "../../timer_info.hpp"
#include "../../timer_manager.hpp"
#include "../../../common/logger.hpp"

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
        {
            logger::getInstance().logWarning("Failed to deserialize header: Total size too small (" + std::to_string(total_size) + ")");
            return false;
        }

        out_header.seq_no = ntohl(*reinterpret_cast<const uint32_t *>(buf + 0));
        out_header.ack_no = ntohl(*reinterpret_cast<const uint32_t *>(buf + 4));
        out_header.win_sz = ntohs(*reinterpret_cast<const uint16_t *>(buf + 8));
        out_header.flags = ntohs(*reinterpret_cast<const uint16_t *>(buf + 10));
        out_header.checksum = ntohs(*reinterpret_cast<const uint16_t *>(buf + 12));

        if (!verify_checksum(buf, total_size, out_header.checksum))
        {
            logger::getInstance().logWarning("Failed to deserialize header: Checksum mismatch. Packet dropped.");
            return false;
        }
        return true;
    }

    static void serialize_header(char *buf, const channel_header &header)
    {
        uint32_t seq = htonl(header.seq_no);
        uint32_t ack = htonl(header.ack_no);
        uint16_t win = htons(header.win_sz);
        uint16_t flg = htons(header.flags);
        uint16_t zero = 0; // Placeholder for checksum

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
            if (i == 12) // Skip checksum field
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
        logger::getInstance().logInfo("Circular buffer created with capacity: " + std::to_string(size));
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
        logger::getInstance().logTest("Wrote " + std::to_string(len) + " bytes to buffer at global pos " + std::to_string(position));
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
        logger::getInstance().logTest("Read " + std::to_string(len) + " bytes from buffer at global pos " + std::to_string(position));
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
        uint32_t old_val = seq_no.fetch_add(delta, std::memory_order_release);
        logger::getInstance().logTest("Sequence advanced by " + std::to_string(delta) + " from " + std::to_string(old_val) + " to " + std::to_string(old_val + delta));
        return old_val;
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
        uint16_t old_win = remote_window.load(std::memory_order_acquire);
        remote_window.store(window_size, std::memory_order_release);
        logger::getInstance().logInfo("Remote window updated from " + std::to_string(old_win) + " to " + std::to_string(window_size));
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
        logger::getInstance().logTest("Added segment to retransmission queue: Seq=" + std::to_string(seq_no) + ", Len=" + std::to_string(length));
    }

    bool get_retransmit_segment(inflight_segment &out_seg)
    {
        std::lock_guard<std::mutex> guard(lock);
        uint64_t now = get_current_time_ms();

        if (segments.empty())
        {
            return false;
        }

        for (auto &seg : segments)
        {
            if ((now - seg.last_sent_time) > ordered_channel_config::RTO_MS)
            {
                if (seg.retransmit_count >= ordered_channel_config::MAX_RETRANSMITS)
                {
                    logger::getInstance().logCritical("Segment " + std::to_string(seg.seq_no) + " reached max retransmits (" + std::to_string(ordered_channel_config::MAX_RETRANSMITS) + "). Connection failure likely.");
                    // In a real application, you'd likely trigger a connection termination here.
                    continue;
                }

                seg.last_sent_time = now;
                seg.retransmit_count++;
                out_seg = seg;
                logger::getInstance().logWarning("Retransmitting segment: Seq=" + std::to_string(out_seg.seq_no) + ", Retransmit Count=" + std::to_string(out_seg.retransmit_count));
                return true;
            }
        }
        return false;
    }

    void acknowledge_up_to(uint32_t ack_no)
    {
        std::lock_guard<std::mutex> guard(lock);
        uint32_t acknowledged_bytes = 0;
        while (!segments.empty() &&
               segments.front().seq_no + segments.front().length <= ack_no)
        {
            acknowledged_bytes += segments.front().length;
            segments.pop_front();
        }
        if (acknowledged_bytes > 0)
        {
            logger::getInstance().logInfo("Acknowledged " + std::to_string(acknowledged_bytes) + " bytes. New SND_UNA=" + std::to_string(ack_no));
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
        {
            logger::getInstance().logWarning("Packet failed header deserialization (checksum fail or malformed).");
            return false;
        }

        uint32_t payload_len = packet_size - ordered_channel_config::HEADER_SIZE;
        logger::getInstance().logTest("Received packet: Seq=" + std::to_string(out_header.seq_no) + ", Ack=" + std::to_string(out_header.ack_no) + ", Win=" + std::to_string(out_header.win_sz) + ", PayloadLen=" + std::to_string(payload_len));

        if (payload_len == 0 && (out_header.flags & static_cast<uint16_t>(channel_flags::ACK)) == 0)
            return true; // No data, no ACK flag. Keepalive?

        std::lock_guard<std::mutex> guard(lock);

        if (payload_len == 0)
            return true; // ACK-only packet

        const char *payload = packet + ordered_channel_config::HEADER_SIZE;
        uint32_t seg_start = out_header.seq_no;
        uint32_t seg_end = out_header.seq_no + payload_len;
        uint32_t current_rcv_nxt = rcv_nxt.get();
        uint32_t app_read_pos = app_read_seq.get();

        if (seg_end <= app_read_pos)
        {
            logger::getInstance().logInfo("Packet " + std::to_string(seg_start) + " is old/already read. Sending ACK.");
            ack_pending.store(true, std::memory_order_release);
            return true;
        }

        if (seg_start >= app_read_pos + buffer.capacity())
        {
            logger::getInstance().logWarning("Packet " + std::to_string(seg_start) + " is outside receive window (overflow). Dropped.");
            return false;
        }

        // This check is the essence of reliability and ordering:
        if (seg_start == current_rcv_nxt)
        {
            // In-order segment
            buffer.write_at_position(current_rcv_nxt, payload, payload_len);
            rcv_nxt.advance(payload_len);
            logger::getInstance().logInfo("Received in-order segment: Seq=" + std::to_string(seg_start) + ". New RCV_NXT=" + std::to_string(rcv_nxt.get()));
            advance_with_sack();
            ack_pending.store(true, std::memory_order_release);
        }
        else if (seg_start > current_rcv_nxt)
        {
            // Out-of-order segment
            if (out_of_order_segments.find(seg_start) == out_of_order_segments.end())
            {
                out_of_order_segments[seg_start] = std::vector<char>(payload, payload + payload_len);
                logger::getInstance().logWarning("Received out-of-order segment: Seq=" + std::to_string(seg_start) + ". Stored.");
            }
            else
            {
                logger::getInstance().logWarning("Received duplicate out-of-order segment: Seq=" + std::to_string(seg_start) + ". Dropped duplicate.");
            }
            ack_pending.store(true, std::memory_order_release);
        }
        else // seg_start < current_rcv_nxt
        {
            // Duplicate or partial overlap of already received data
            if (seg_end > current_rcv_nxt)
            {
                // Partial overlap (retransmission of unacknowledged data)
                uint32_t overlap = current_rcv_nxt - seg_start;
                uint32_t new_data_len = seg_end - current_rcv_nxt;
                buffer.write_at_position(current_rcv_nxt, payload + overlap, new_data_len);
                rcv_nxt.advance(new_data_len);
                logger::getInstance().logWarning("Received partial overlap (retrans): Seg=" + std::to_string(seg_start) + ", New Data=" + std::to_string(new_data_len) + ". New RCV_NXT=" + std::to_string(rcv_nxt.get()));
                advance_with_sack();
            }
            else
            {
                logger::getInstance().logInfo("Received full duplicate segment: Seq=" + std::to_string(seg_start) + ". Ignored data.");
            }
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
        {
            logger::getInstance().logTest("Application read: 0 bytes available.");
            return 0;
        }

        uint32_t to_read = std::min(available, len);
        buffer.read_from_position(current_app_read, out_buf, to_read);
        app_read_seq.advance(to_read);
        logger::getInstance().logInfo("Application read: " + std::to_string(to_read) + " bytes. New APP_READ_SEQ=" + std::to_string(app_read_seq.get()));

        return static_cast<ssize_t>(to_read);
    }

    uint32_t get_ack_no() const { return rcv_nxt.get(); }

    uint16_t get_window_size() const
    {
        uint32_t used = rcv_nxt.get() - app_read_seq.get();
        uint32_t available = (used < buffer.capacity()) ? (buffer.capacity() - used) : 0;
        return static_cast<uint16_t>(std::min(available, static_cast<uint32_t>(0xFFFF)));
    }

    bool is_ack_pending() const { return ack_pending.load(std::memory_order_acquire); }
    void clear_ack_pending()
    {
        if (ack_pending.exchange(false, std::memory_order_release))
        {
            logger::getInstance().logTest("Cleared ACK pending flag.");
        }
    }
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
                logger::getInstance().logInfo("SACK-ed segment found: Seq=" + std::to_string(it->first) + ". Advancing RCV_NXT.");
                buffer.write_at_position(current_rcv, it->second.data(), it->second.size());
                current_rcv += it->second.size();
                out_of_order_segments.erase(it);
            }
            else
            {
                break;
            }
        }
        if (current_rcv > rcv_nxt.get())
        {
            rcv_nxt.set(current_rcv);
            logger::getInstance().logInfo("RCV_NXT advanced with SACK consolidation. New RCV_NXT=" + std::to_string(rcv_nxt.get()));
        }
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
        uint32_t total_capacity = buffer.capacity();

        // Safe way to calculate buffered data in a circular buffer
        uint32_t buffered = (current_write - current_una);
        uint32_t available = (total_capacity - 1) - buffered; // -1 to avoid head == tail ambiguity

        if (available < len)
        {
            logger::getInstance().logWarning("Application write failed: Not enough buffer space. Requested " + std::to_string(len) + ", Available " + std::to_string(available));
            return -1;
        }

        buffer.write_at_position(current_write, data, len);
        write_pos.advance(len);
        logger::getInstance().logInfo("Application wrote " + std::to_string(len) + " bytes. New WRITE_POS=" + std::to_string(write_pos.get()));

        return static_cast<ssize_t>(len);
    }

    uint32_t get_packet_to_send(char *out_buf, uint32_t max_payload, uint32_t &out_seq_no)
    {
        // 1. Check for retransmission (highest priority)
        inflight_segment seg;
        if (retransmissions.get_retransmit_segment(seg))
        {
            buffer.read_from_position(seg.seq_no, out_buf, seg.length);
            out_seq_no = seg.seq_no;
            logger::getInstance().logTest("Retransmission data loaded: Seq=" + std::to_string(out_seq_no) + ", Len=" + std::to_string(seg.length));
            return seg.length;
        }

        // 2. Send new data
        uint32_t current_nxt = snd_nxt.get();
        uint32_t current_write = write_pos.get();
        uint32_t current_una = snd_una.get();
        uint32_t available_to_send = current_write - current_nxt; // Bytes written but not yet sent
        uint32_t in_flight = current_nxt - current_una;
        uint16_t remote_win = flow.get_remote_window();

        if (available_to_send == 0)
            return 0;

        if (!flow.can_send_bytes(in_flight))
        {
            logger::getInstance().logTest("Cannot send new data: Remote window is full. InFlight=" + std::to_string(in_flight) + ", RemoteWin=" + std::to_string(remote_win));
            return 0;
        }

        uint32_t window_available = remote_win - in_flight;

        uint32_t can_send = std::min({available_to_send,
                                      (uint32_t)ordered_channel_config::MAX_MSS,
                                      window_available});

        if (can_send == 0)
        {
            logger::getInstance().logTest("Cannot send new data: Effective send size is 0. AvailToWrite=" + std::to_string(available_to_send) + ", WindowAvail=" + std::to_string(window_available));
            return 0;
        }

        buffer.read_from_position(current_nxt, out_buf, can_send);
        retransmissions.add_segment(current_nxt, can_send);
        out_seq_no = current_nxt;
        snd_nxt.advance(can_send);
        logger::getInstance().logInfo("Sending new data: Seq=" + std::to_string(out_seq_no) + ", Len=" + std::to_string(can_send) + ". New SND_NXT=" + std::to_string(snd_nxt.get()));

        return can_send;
    }

    void process_ack(uint32_t ack_no)
    {
        uint32_t current_una = snd_una.get();
        if (ack_no > current_una)
        {
            logger::getInstance().logInfo("Processing ACK: " + std::to_string(ack_no) + ". Current SND_UNA: " + std::to_string(current_una));
            snd_una.set(ack_no);
            retransmissions.acknowledge_up_to(ack_no);
        }
        else if (ack_no < current_una)
        {
            logger::getInstance().logWarning("Received old ACK: " + std::to_string(ack_no) + ". Current SND_UNA: " + std::to_string(current_una));
        }
        else
        {
            logger::getInstance().logTest("Received duplicate ACK: " + std::to_string(ack_no));
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
        logger::getInstance().logWarning("RTO timer expired. Triggering retransmission check.");
        auto pkt = on_transport_send();
        if (pkt && on_net_data_ready)
            on_net_data_ready(std::move(pkt));

        // Reschedule RTO to keep checking for unacknowledged data
        schedule_rto_timer();
    }

    void on_delayed_ack_expire()
    {
        logger::getInstance().logTest("Delayed ACK timer expired. Checking for pending ACK.");
        // Flush pending ACK
        if (rcv_window.is_ack_pending())
        {
            logger::getInstance().logInfo("Delayed ACK pending. Sending ACK packet.");
            auto pkt = on_transport_send();
            if (pkt && on_net_data_ready)
                on_net_data_ready(std::move(pkt));
        }
        else
        {
            logger::getInstance().logTest("Delayed ACK not pending. No packet sent.");
        }
    }

    // === Timer Scheduling ===
    void schedule_rto_timer()
    {
        if (!global_timer_manager)
        {
            logger::getInstance().logError("RTO Timer not scheduled: Timer Manager not set.");
            return;
        }

        auto cb = [this]()
        { this->on_rto_expire(); };
        auto timer = std::make_unique<timer_info>(duration_ms(ordered_channel_config::RTO_MS), cb);
        global_timer_manager->add_timer(std::move(timer));
        logger::getInstance().logTest("RTO timer scheduled for " + std::to_string(ordered_channel_config::RTO_MS) + "ms.");
    }

    void schedule_delayed_ack_timer()
    {
        if (!global_timer_manager)
        {
            logger::getInstance().logError("Delayed ACK Timer not scheduled: Timer Manager not set.");
            return;
        }

        // NOTE: This implementation assumes a simple timer that can be overwritten
        // or that the timer manager handles duplicate timers appropriately (e.g., uses a key/ID).
        // Since we don't have a timer ID, we just add a new one.
        auto cb = [this]()
        { this->on_delayed_ack_expire(); };
        auto timer = std::make_unique<timer_info>(duration_ms(DELAYED_ACK_MS), cb);
        global_timer_manager->add_timer(std::move(timer));
        logger::getInstance().logTest("Delayed ACK timer scheduled for " + std::to_string(DELAYED_ACK_MS) + "ms.");
    }

public:
    explicit reliable_ordered_channel(channel_id id) : ch_id(id)
    {
        logger::getInstance().logInfo("Reliable Ordered Channel " + std::to_string(ch_id) + " created.");
    }

    std::unique_ptr<i_channel> clone() const override
    {
        logger::getInstance().logError("Cloning Reliable Ordered Channel not supported.");
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
            return;

        const char *ibuf = buf + off;
        uint32_t sz = static_cast<uint32_t>(len - off);

        channel_header header{};
        if (!rcv_window.receive_packet(ibuf, sz, header))
        {
            logger::getInstance().logWarning("Failed to process received packet in receive window. Dropped.");
            return;
        }

        if ((header.flags & static_cast<uint16_t>(channel_flags::ACK)) != 0)
            snd_window.process_ack(header.ack_no);

        if ((header.flags & static_cast<uint16_t>(channel_flags::WIND_SZ)) != 0)
            snd_window.process_window_update(header.win_sz);

        uint32_t payload_len = (sz > ordered_channel_config::HEADER_SIZE) ? (sz - ordered_channel_config::HEADER_SIZE) : 0;

        if (payload_len > 0)
        {
            logger::getInstance().logInfo("Received data payload (" + std::to_string(payload_len) + " bytes). Notifying application.");
            if (on_app_data_ready)
                on_app_data_ready();
        }

        // Schedule delayed ACK on data receipt
        if (rcv_window.is_ack_pending())
        {
            if (payload_len > 0)
            {
                schedule_delayed_ack_timer();
            }
            else
            {
                // For ACK-only packets that update RCV_NXT (e.g., initial ACK), send immediate ACK back.
                // Or if an ACK-only was received, we still respond to update our window.
                auto pkt_to_send = on_transport_send();
                if (pkt_to_send && on_net_data_ready)
                {
                    logger::getInstance().logTest("Received ACK-only packet, sending response immediately.");
                    on_net_data_ready(std::move(pkt_to_send));
                }
            }
        }
    }

    std::unique_ptr<rudp_protocol_packet> on_transport_send() override
    {
        uint32_t seq_no = 0;
        char payload_buf[ordered_channel_config::MAX_MSS];
        uint32_t max_payload = ordered_channel_config::MAX_MSS;
        uint32_t payload_len = snd_window.get_packet_to_send(payload_buf, max_payload, seq_no);

        bool send_ack = flags.get_send_ack() || rcv_window.is_ack_pending();
        bool send_window = flags.get_send_window();

        if (payload_len == 0 && !send_ack && !send_window)
        {
            logger::getInstance().logTest("on_transport_send: No data to send, no ACK/Window update needed. Returning nullptr.");
            return nullptr;
        }

        uint32_t total_size = rudp_protocol::CHANNEL_HEADER_OFFSET +
                              ordered_channel_config::HEADER_SIZE + payload_len;
        auto packet = std::make_unique<rudp_protocol_packet>(total_size);
        packet->set_length(total_size);

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

        uint16_t csum_val = packet_codec::calculate_checksum(pkt_buf,
                                                             ordered_channel_config::HEADER_SIZE + payload_len);
        uint16_t final_csum = htons(~csum_val);
        memcpy(pkt_buf + 12, &final_csum, 2);

        rcv_window.clear_ack_pending();

        std::string log_msg = "Sending packet: Seq=" + std::to_string(seq_no) +
                              ", Ack=" + std::to_string(header.ack_no) +
                              ", Win=" + std::to_string(header.win_sz);
        if (payload_len > 0)
            log_msg += ", Payload=" + std::to_string(payload_len) + " bytes (Data)";
        if (send_ack)
            log_msg += " (ACK)";
        if (send_window)
            log_msg += " (WIND_SZ)";
        logger::getInstance().logInfo(log_msg);

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
            logger::getInstance().logInfo("Application wrote " + std::to_string(written) + " bytes. Attempting to send immediately.");
            auto pkt = on_transport_send();
            if (pkt && on_net_data_ready)
                on_net_data_ready(std::move(pkt));
        }
        else
        {
            logger::getInstance().logWarning("Application write failed (buffer full).");
        }
        return written;
    }

    void set_on_app_data_ready(std::function<void()> f) override
    {
        on_app_data_ready = f;
        logger::getInstance().logTest("on_app_data_ready callback set.");
    }

    void set_on_net_data_ready(std::function<void(std::unique_ptr<rudp_protocol_packet>)> f) override
    {
        on_net_data_ready = f;
        logger::getInstance().logTest("on_net_data_ready callback set.");
    }

    void set_timer_manager(std::shared_ptr<timer_manager> timer_man) override
    {
        global_timer_manager = timer_man;
        logger::getInstance().logInfo("Timer manager set. Scheduling initial timers.");
        schedule_rto_timer();
    }
};