#pragma once
#include "core_logic.hpp"

// ---------------------------------------------------------
// RECEIVE WINDOW
// ---------------------------------------------------------
class receive_window
{
    char *buf;
    uint32_t buf_size;
    uint16_t rcv_window_size;

    // Sequence numbers
    uint32_t app_read_seq; // App reads from here
    uint32_t rcv_nxt;      // Network expects this (Cumulative ACK)

    sack_logic sack_handler;

    uint16_t deserialize_header(const char *buf, rudp_header &h)
    {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < HEADER_SIZE; i += 2)
        {
            sum += (static_cast<uint8_t>(buf[i]) << 8) | static_cast<uint8_t>(buf[i + 1]);
        }
        h.seq_no = ntohl(*reinterpret_cast<const uint32_t *>(buf));
        h.ack_no = ntohl(*reinterpret_cast<const uint32_t *>(buf + 4));
        h.win_sz = ntohs(*reinterpret_cast<const uint16_t *>(buf + 8));
        h.flags = ntohs(*reinterpret_cast<const uint16_t *>(buf + 10));
        h.checksum = ntohs(*reinterpret_cast<const uint16_t *>(buf + 12));
        return static_cast<uint16_t>(fold_and_accumulate(sum));
    }

    uint16_t deserialize_payload_and_write(const char *ibuf, const uint32_t &sz, uint32_t segment_seq_no)
    {
        uint32_t sum = 0;
        uint32_t words = sz / 2;
        for (uint32_t i = 0; i < words; ++i)
        {
            uint32_t offset = i * 2;
            uint16_t high = static_cast<uint8_t>(ibuf[offset]);
            uint16_t low = static_cast<uint8_t>(ibuf[offset + 1]);
            sum += (high << 8) | low;

            uint32_t abs_seq = segment_seq_no + offset;
            buf[abs_seq % buf_size] = static_cast<char>(high);
            buf[(abs_seq + 1) % buf_size] = static_cast<char>(low);
        }
        if (sz & 1)
        {
            uint8_t last = ibuf[sz - 1];
            sum += (static_cast<uint16_t>(last) << 8);
            buf[(segment_seq_no + sz - 1) % buf_size] = static_cast<char>(last);
        }
        return static_cast<uint16_t>(fold_and_accumulate(sum));
    }

public:
    receive_window(size_t size = DEFAULT_BUFFER_SIZE)
        : buf(new char[size]), buf_size(size), rcv_window_size(size)
    {
        app_read_seq = get_random_uint64_t();
        rcv_nxt = app_read_seq;
    }
    ~receive_window() { delete[] buf; }

    // Returns > 0 (payload size) if packet valid, 0 if dropped/invalid
    uint32_t receive_packet(const char *pkt_buf, const uint32_t &sz, rudp_header &h_out)
    {
        if (sz < HEADER_SIZE)
            return 0;

        rudp_header h;
        uint16_t csum_l = deserialize_header(pkt_buf, h);
        uint16_t csum_r = deserialize_payload_and_write(pkt_buf + HEADER_SIZE, sz - HEADER_SIZE, h.seq_no);

        if (static_cast<uint16_t>(fold_and_accumulate(uint32_t(csum_l) + csum_r)) != 0xFFFF)
            return 0;

        // Output header for the Channel to use (e.g. to update sender remote window)
        h_out = h;

        sack_handler.incoming_segment(h.seq_no, h.seq_no + (sz - HEADER_SIZE), rcv_nxt);

        // Update Window Size
        uint32_t highest_seq = rcv_nxt;
        if (!sack_handler.sack_blocks.empty())
            highest_seq = sack_handler.sack_blocks.rbegin()->second;
        uint32_t span = highest_seq - app_read_seq;
        rcv_window_size = (span < buf_size) ? (buf_size - span) : 0;

        return sz;
    }

    uint32_t read_from_buffer(char *obuf, const uint32_t &sz)
    {
        if (rcv_nxt <= app_read_seq)
            return 0;
        uint32_t to_read = std::min(rcv_nxt - app_read_seq, sz);
        uint32_t start = app_read_seq % buf_size;
        uint32_t chunk1 = std::min(to_read, buf_size - start);

        memcpy(obuf, buf + start, chunk1);
        if (chunk1 < to_read)
            memcpy(obuf + chunk1, buf, to_read - chunk1);

        app_read_seq += to_read;

        // Recalculate window
        uint32_t highest = rcv_nxt;
        if (!sack_handler.sack_blocks.empty())
            highest = sack_handler.sack_blocks.rbegin()->second;
        uint32_t span = highest - app_read_seq;
        rcv_window_size = (span < buf_size) ? (buf_size - span) : 0;
        return to_read;
    }

    uint32_t get_ack_no() const { return rcv_nxt; }
    uint16_t get_win_sz() const { return rcv_window_size; }
};

// ---------------------------------------------------------
// SEND WINDOW
// ---------------------------------------------------------
class send_window
{
    char *buf;
    uint32_t buf_size;
    uint16_t remote_win_sz;

    // Indices (0 to buf_size)
    uint32_t sent_not_acked_idx;
    uint32_t can_be_sent_idx;
    uint32_t can_be_written_idx;

    // Sequence Numbers
    uint32_t snd_una; // Sequence number of sent_not_acked_idx
    uint32_t snd_nxt; // Sequence number of can_be_sent_idx

    uint16_t serialize_header(char *out_buf, uint32_t ack_no, uint16_t win_sz)
    {
        rudp_header h{snd_nxt, ack_no, win_sz, 0, 0};
        uint32_t seq = htonl(h.seq_no), ack = htonl(h.ack_no);
        uint16_t win = htons(h.win_sz), flg = htons(h.flags), zero = 0;

        memcpy(out_buf, &seq, 4);
        memcpy(out_buf + 4, &ack, 4);
        memcpy(out_buf + 8, &win, 2);
        memcpy(out_buf + 10, &flg, 2);
        memcpy(out_buf + 12, &zero, 2);

        uint32_t sum = 0;
        for (int i = 0; i < HEADER_SIZE; i += 2)
            sum += (static_cast<uint8_t>(out_buf[i]) << 8) | static_cast<uint8_t>(out_buf[i + 1]);
        return static_cast<uint16_t>(fold_and_accumulate(sum));
    }

    uint16_t serialize_payload(char *out_buf, const uint32_t &sz)
    {
        uint32_t sum = 0, words = sz / 2;
        for (uint32_t i = 0; i < words; ++i)
        {
            uint32_t idx = (can_be_sent_idx + (i * 2)) % buf_size;
            uint16_t h = static_cast<uint8_t>(buf[idx]);
            uint16_t l = static_cast<uint8_t>(buf[(idx + 1) % buf_size]);
            sum += (h << 8) | l;
            out_buf[i * 2] = h;
            out_buf[i * 2 + 1] = l;
        }
        if (sz & 1)
        {
            uint8_t last = buf[(can_be_sent_idx + sz - 1) % buf_size];
            sum += (static_cast<uint16_t>(last) << 8);
            out_buf[sz - 1] = last;
        }
        return static_cast<uint16_t>(fold_and_accumulate(sum));
    }

public:
    send_window(size_t size = DEFAULT_BUFFER_SIZE)
        : buf(new char[size]), buf_size(size), remote_win_sz(10000),
          sent_not_acked_idx(0), can_be_sent_idx(0), can_be_written_idx(0),
          snd_una(0), snd_nxt(0) {}
    ~send_window() { delete[] buf; }

    void process_ack(uint32_t ack_no)
    {
        // Calculate bytes acknowledged (handling wrap-around via unsigned math)
        uint32_t bytes_acked = ack_no - snd_una;
        // Basic sanity check: Ack should be <= what we sent
        if (bytes_acked > (snd_nxt - snd_una))
            return; // Invalid ACK (future)

        sent_not_acked_idx = (sent_not_acked_idx + bytes_acked) % buf_size;
        snd_una = ack_no;
    }

    void set_remote_window(uint16_t w) { remote_win_sz = w; }

    uint32_t send_packet(char *pkt_buf, uint32_t ack_to_send, uint16_t win_to_send)
    {
        uint32_t buffered = (can_be_written_idx - can_be_sent_idx + buf_size) % buf_size;
        uint32_t inflight = (can_be_sent_idx - sent_not_acked_idx + buf_size) % buf_size;

        if (inflight >= remote_win_sz)
            return 0;
        uint32_t allowed = std::min(buffered, (uint32_t)(remote_win_sz - inflight));
        uint32_t payload_sz = std::min(allowed, (uint32_t)MAX_MSS);

        if (payload_sz == 0)
            return 0;

        uint16_t csum_l = serialize_header(pkt_buf, ack_to_send, win_to_send);
        uint16_t csum_r = serialize_payload(pkt_buf + HEADER_SIZE, payload_sz);
        uint16_t final = htons(~static_cast<uint16_t>(fold_and_accumulate(uint32_t(csum_l) + csum_r)));
        memcpy(pkt_buf + 12, &final, 2);

        snd_nxt += payload_sz;
        can_be_sent_idx = (can_be_sent_idx + payload_sz) % buf_size;

        return HEADER_SIZE + payload_sz;
    }

    uint32_t write_into_buffer(const char *ibuf, const uint32_t &sz)
    {
        uint32_t used = (can_be_written_idx - sent_not_acked_idx + buf_size) % buf_size;
        uint32_t free = buf_size - 1 - used;
        uint32_t to_write = std::min(free, sz);
        uint32_t chunk1 = std::min(to_write, buf_size - can_be_written_idx);

        memcpy(buf + can_be_written_idx, ibuf, chunk1);
        if (chunk1 < to_write)
            memcpy(buf, ibuf + chunk1, to_write - chunk1);

        can_be_written_idx = (can_be_written_idx + to_write) % buf_size;
        return to_write;
    }
};