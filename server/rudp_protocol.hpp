#pragma once

#include <algorithm>
#include <cstddef>

#include "raw_packet.hpp"
#include "types.hpp"
#include "channel_manager.hpp"
#include "i_session_control_callback.hpp"

// out one packet will be at max 16 bits, coz udp

namespace rudp_protocol
{

    // offsets

    constexpr size_t COMMON_HEADER_SIZE = 10;
    constexpr size_t COMMON_HEADER_OFFSET = 0;
    constexpr size_t CHANNEL_HEADER_OFFSET = 10;

    //

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

    //
    struct common_header
    {

        static constexpr size_t CLIENT_ID_OFSET = 0, CHANNEL_ID_OFFSET = 8;

        uint64_t cl_id;
        uint16_t ch_id;

        common_header() = default;
        common_header(uint64_t cli_id, uint64_t cha_id) : cl_id(cli_id), ch_id(cha_id) {}

        common_header &operator=(const i_packet &pkt)
        {
            // here deserialize pkt
            const char *buf = pkt.get_const_buffer();
            buf += COMMON_HEADER_OFFSET;

            //
            cl_id = ntohll(*reinterpret_cast<const uint64_t *>(buf));
            buf += 8;

            ch_id = ntohs(*reinterpret_cast<const uint16_t *>(buf));

            return *this;
        }
    };
    // so common header is 96 bits i.e 12 bytes

    class session_control
    {

        //
        std::weak_ptr<i_session_control_callback> channel_manager_;

    public:
        // for channel manager
        void handle_control_packet(const client_id &cl_id, std::unique_ptr<i_sockaddr> source_addr, const char *ibuf, const uint32_t &sz)
        {
        }
        //

        // for creaotr
        void set_channel_manager(std::weak_ptr<i_session_control_callback> ch_manager)
        {
            channel_manager_ = ch_manager;
        }
    };
};