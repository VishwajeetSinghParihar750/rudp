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

    // constants

    constexpr size_t COMMON_HEADER_SIZE = 10;
    constexpr size_t COMMON_HEADER_OFFSET = 0;
    constexpr size_t CHANNEL_HEADER_OFFSET = 10;

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

enum class CONNECTION_STATE
{
    CLOSED,
    LISTEN,
    SYN_RCVD,
    ESTABLISHED,
    CLOSE_WAIT,
    LAST_ACK,
    FIN_WAIT1,
    FIN_WAIT2,
    CLOSING,
    TIME_WAIT
};

enum class CONNECTION_PACKET_FLAGS : uint8_t
{
    SYN = 1,
    ACK = (1 << 1),
    RST = (1 << 2),
    FIN = (1 << 3)
};

struct CONNECTION_STATE_MACHINE
{
    CONNECTION_STATE current_state = CONNECTION_STATE::LISTEN;

    void send_syn_ack() {}
    void send_ack() {}
    void send_rst() {}
    void send_fin() {}

    void close()
    {
        if (current_state == CONNECTION_STATE::SYN_RCVD)
        {
            send_rst();
            current_state = CONNECTION_STATE::CLOSED;
        }
        else if (current_state == CONNECTION_STATE::ESTABLISHED)
        {
            send_fin();
            current_state = CONNECTION_STATE::FIN_WAIT1;
        }
        else if (current_state == CONNECTION_STATE::CLOSE_WAIT)
        {
            send_fin();
            current_state = CONNECTION_STATE::LAST_ACK;
        }
        // else ignore
    }

    void handle_change(uint8_t rcvd_flags)
    {
        if (rcvd_flags & (uint8_t)CONNECTION_PACKET_FLAGS::RST)
        {
            if (current_state != CONNECTION_STATE::LISTEN && current_state != CONNECTION_STATE::CLOSED)
            {
                current_state = CONNECTION_STATE::CLOSED;
                return;
            }
        }

        switch (current_state)
        {
        case CONNECTION_STATE::LISTEN:
        {

            if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::SYN)
            {
                send_syn_ack();
                current_state = CONNECTION_STATE::SYN_RCVD;
            }
            else
                send_rst();

            break;
        }

        case CONNECTION_STATE::SYN_RCVD:
        {
            if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::RST)
            {
                current_state = CONNECTION_STATE::CLOSED;
            }
            else if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::ACK)
            {
                current_state = CONNECTION_STATE::ESTABLISHED;
            }
            else
                send_rst();
            break;
        }
        case CONNECTION_STATE::ESTABLISHED:
        {
            if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::FIN)
            {
                send_ack();
                current_state = CONNECTION_STATE::CLOSE_WAIT;
            }
            else
                send_rst();

            break;
        }
        case CONNECTION_STATE::LAST_ACK:
        {
            if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::ACK)
            {
                current_state = CONNECTION_STATE::CLOSED;
            }
            else
                send_rst();

            break;
        }
        case CONNECTION_STATE::FIN_WAIT1:
        {
            if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::ACK)
            {
                current_state = CONNECTION_STATE::FIN_WAIT2;
            }
            else if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::FIN)
            {
                send_ack();
                current_state = CONNECTION_STATE::CLOSING;
            }
            else if (rcvd_flags == ((uint8_t)CONNECTION_PACKET_FLAGS::FIN | (uint8_t)CONNECTION_PACKET_FLAGS::ACK))
            {
                send_ack();
                current_state = CONNECTION_STATE::TIME_WAIT;
            }
            else
                send_rst();

            break;
        }
        case CONNECTION_STATE::FIN_WAIT2:
        {
            if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::FIN)
            {
                send_ack();
                current_state = CONNECTION_STATE::TIME_WAIT;
            }
            else
                send_rst();

            break;
        }
        case CONNECTION_STATE::CLOSING:
        {
            if (rcvd_flags == (uint8_t)CONNECTION_PACKET_FLAGS::ACK)
            {
                current_state = CONNECTION_STATE::TIME_WAIT;
            }

            else
                send_rst();

            break;
        }
        default:
            break;
        }
    }
};