#pragma once

#include <iostream>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>

// --- Constants & Utils ---
static constexpr uint16_t HEADER_SIZE = 14;
static constexpr uint32_t DEFAULT_BUFFER_SIZE = 4096;
static constexpr uint16_t MAX_MSS = 1400;

inline uint32_t fold_and_accumulate(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return sum;
}

// --- Shared Header Structure ---
struct rudp_header
{
    uint32_t seq_no;
    uint32_t ack_no;
    uint16_t win_sz;
    uint16_t flags; // ack = this has an ack,  is_last = this is the last segment  ...
    uint16_t checksum;
};

// --- SACK Logic ---
class sack_logic
{
public:
    using sequence_number = uint32_t;
    std::map<sequence_number, sequence_number> sack_blocks;

    bool should_send_ack = false;
    // Returns true if SACK state was updated
    void incoming_segment(sequence_number start_seq, sequence_number end_seq, sequence_number &rcv_nxt)
    {
        if (start_seq < rcv_nxt)
            start_seq = rcv_nxt;
        if (start_seq >= end_seq)
            return;

        //

        sequence_number new_start = start_seq;
        sequence_number new_end = end_seq;

        // 1. Find and merge overlapping blocks
        auto it = sack_blocks.upper_bound(new_start);
        if (it != sack_blocks.begin())
        {
            --it;
            if (it->second < new_start)
                ++it;
        }

        std::vector<sequence_number> to_erase;
        auto current = it;

        while (current != sack_blocks.end() && current->first <= new_end)
        {
            if (current->second >= new_start)
            {
                new_start = std::min(new_start, current->first);
                new_end = std::max(new_end, current->second);
                to_erase.push_back(current->first);
            }
            ++current;
        }

        for (auto key : to_erase)
            sack_blocks.erase(key);
        sack_blocks[new_start] = new_end;

        // 2. Advance RCV.NXT if contiguous
        check_and_advance_rcv_nxt(rcv_nxt);
    }

private:
    void check_and_advance_rcv_nxt(sequence_number &rcv_nxt)
    {
        if (sack_blocks.empty())
            return;
        auto first = sack_blocks.begin();
        if (first->first == rcv_nxt)
        {
            rcv_nxt = first->second;
            sack_blocks.erase(first);
            check_and_advance_rcv_nxt(rcv_nxt);
        }
    }
};