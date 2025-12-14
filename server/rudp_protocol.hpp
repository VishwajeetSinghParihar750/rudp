#pragma once

// out one packet will be at max 16 bits, coz udp
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>

#include "types.hpp"

namespace rudp_protocol
{

    struct channel_manager_header
    {
        channel_id ch_id;
        channel_manager_header(const channel_id &id) : ch_id(id) {}
    };
    struct session_control_header
    {
        uint8_t flags;
        uint32_t reserved;
    };

    // constants

    constexpr size_t SESSION_CONTROL_HEADER_OFFSET = 0;
    constexpr size_t SESSION_CONTROL_HEADER_SIZE = 5;

    constexpr size_t CHANNEL_MANAGER_HEADER_OFFSET = 5;
    constexpr size_t CHANNEL_MANAGER_HEADER_SIZE = 4;

    constexpr size_t CHANNEL_HEADER_OFFSET = 9;

    // helpers

    inline uint64_t htonll(uint64_t hostval)
    {
#if __BYTE_ORDER__ == __BIG_ENDIAN
        return hostval;
#else
        uint32_t l = hostval >> 32, r = (hostval & ((1ll << 32) - 1));
        l = htonl(l), r = htonl(r);
        return (uint64_t(r) << 32) | l;

#endif
    }
    inline uint64_t ntohll(uint64_t netval) { return htonll(netval); }

};
