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
    constexpr size_t SESSION_CONTROL_HEADER_SIZE = 2;

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

    constexpr size_t CONTROL_HEADER_SIZE = 2;

    struct control_header
    {
        uint8_t flags;
        // syn, ack, fin, rst, sum = reserved bits are checksum , cnl = has channel info in payload
        uint8_t reserved;

        control_header() = default;
        control_header(uint8_t f) : flags(f), reserved(0) {}
    };

    class session_control
    {
        std::weak_ptr<i_session_control_callback> data_channel_manager;
        std::unordered_map<client_id, std::unique_ptr<reliable_ordered_channel>> clients_control_channels;

        static void serialize_control_header(char *buf, const size_t &len, const control_header &ss_header)
        {
            assert(len >= CONTROL_HEADER_SIZE);

            memcpy(buf, &ss_header.flags, sizeof(uint8_t));
            memcpy(buf + sizeof(uint8_t), &ss_header.reserved, sizeof(uint8_t));
        }
        static control_header deserialize_control_header(const char *buf, const uint32_t &len)
        {
            assert(len >= CONTROL_HEADER_SIZE);
            control_header ss_header;

            memcpy(&ss_header.flags, buf, sizeof(uint8_t));
            memcpy(&ss_header.reserved, buf + sizeof(uint8_t), sizeof(uint8_t));

            return ss_header;
        }
        static inline uint8_t calculate_checksum(const char *data, size_t len)
        {
            uint8_t sum = 0;
            for (size_t i = 0; i < len; ++i)
            {
                sum += static_cast<uint8_t>(data[i]);
            }
            return sum;
        }

        client_id get_new_client_id()
        {
            while (true)
            {
                client_id cl_id = get_random_client_id();
                if (cl_id != INVALID_CLIENT_ID && !clients_control_channels.contains(cl_id))
                    return cl_id;
            }
        }

        static std::vector<channel_setup_info> deserialize_channel_setup_info(const char *buf, const uint32_t &len)
        {
            // it shoudl start with 16 bits channel count, then it should have [ chanel id , len, payload   ] ...

            std::vector<channel_setup_info> result;
            const char *ptr = buf;
            const char *end = buf + len;

            if (len < sizeof(uint16_t))
                return result;

            uint16_t count;
            memcpy(&count, ptr, sizeof(uint16_t));
            count = ntohs(count);
            ptr += sizeof(uint16_t);

            result.reserve(count);

            for (uint16_t i = 0; i < count; ++i)
            {
                if (ptr + 2 * sizeof(uint16_t) > end)
                    break;

                channel_setup_info info;
                uint16_t n_ch_id, n_len;

                memcpy(&n_ch_id, ptr, sizeof(uint16_t));
                info.ch_id = ntohs(n_ch_id);
                ptr += sizeof(uint16_t);

                memcpy(&n_len, ptr, sizeof(uint16_t));
                uint16_t payload_len = ntohs(n_len);
                ptr += sizeof(uint16_t);

                if (ptr + payload_len > end)
                    break;

                info.payload = std::make_unique<raw_packet>(payload_len);
                if (payload_len > 0)
                {
                    memcpy(info.payload->get_buffer(), ptr, payload_len);
                }

                result.push_back(std::move(info));
                ptr += payload_len;
            }

            return result;
        }
        static void serialize_channel_setup_info(const channel_id &ch_id, const char *ibuf, char *obuf, const uint16_t &payload_len)
        {
            uint16_t n_ch_id = htons(ch_id);
            uint32_t offset = 0;
            memcpy(obuf + offset, &n_ch_id, sizeof(uint16_t));
            offset += sizeof(uint16_t);

            uint16_t n_len = htons(payload_len);
            memcpy(obuf + offset, &n_len, sizeof(uint16_t));
            offset += sizeof(uint16_t);

            if (payload_len > 0 && ibuf != nullptr)
            {
                memcpy(obuf + offset, ibuf, payload_len);
                offset += payload_len;
            }
        }

        void add_client(const client_id &cl_id)
        {
            clients_control_channels.emplace(cl_id);
        }
        void remove_client(const client_id &cl_id)
        {
            clients_control_channels.erase(cl_id);
        }

    public:
        void handle_control_packet(const client_id &cl_id, bool is_active_connection, std::unique_ptr<i_sockaddr> source_addr, const char *ibuf, const uint32_t &sz)
        {
            if (sz < CONTROL_HEADER_SIZE)
                return;

            control_header cur_header = deserialize_control_header(ibuf, sz);
            const char *payload_ptr = ibuf + CONTROL_HEADER_SIZE;
            uint32_t payload_len = sz - CONTROL_HEADER_SIZE;

            // --- Scenario: New Connection Request ---
            if (cl_id == INVALID_CLIENT_ID && !is_active_connection)
            {
                // 1. Checksum Verification
                if (cur_header.flags & (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SUM)
                {
                    uint8_t checksum = calculate_checksum(ibuf, sz);
                    if (checksum != (~(uint8_t(0))))
                        return;

                    cur_header.flags &= ~(uint8_t)CONTROL_PACKET_HEADER_FLAGS::SUM;
                }

                if (cur_header.flags == ((uint8_t)CONTROL_PACKET_HEADER_FLAGS::SYN | (uint8_t)CONTROL_PACKET_HEADER_FLAGS::CHN))
                {
                    auto manager = data_channel_manager.lock();
                    if (!manager)
                        return;

                    client_id new_cl_id = get_new_client_id();
                    add_client(new_cl_id);
                    manager->add_client(new_cl_id);

                    // ⚠️ need to add more checks to enforce this
                    std::vector<channel_setup_info> channel_setup_info_ = deserialize_channel_setup_info(ibuf + CONTROL_HEADER_SIZE, sz - CONTROL_HEADER_SIZE);

                    if (channel_setup_info_.empty() || channel_setup_info_[0].ch_id != CONTROL_CHANNEL_ID)
                        return; // it must have control channel setup info as first thing

                    std::unique_ptr<raw_packet> ack_pkt = std::make_unique<raw_packet>(1500);
                    uint32_t ack_payload_offset = COMMON_HEADER_SIZE + CONTROL_HEADER_SIZE + sizeof(uint16_t);
                    uint16_t channel_cnt = 0;

                    for (auto &i : channel_setup_info_)
                    {
                        channel_id ch_id = i.ch_id;
                        channel_setup_info res_setup_info;

                        if (ch_id == CONTROL_CHANNEL_ID)
                        {
                            clients_control_channels[new_cl_id]->process_channel_setup_info(std::move(i));
                            res_setup_info = clients_control_channels[new_cl_id]->get_channel_setup_info();
                        }
                        else
                        {
                            manager->add_channel_for_client(new_cl_id, ch_id);
                            manager->process_channel_setup_request(new_cl_id, std::move(i)); //  ℹ️ need error handling here
                            res_setup_info = manager->get_channel_setup_info(new_cl_id, ch_id);
                        }

                        uint32_t size_left = 1500 - ack_payload_offset;
                        uint32_t size_needed = res_setup_info.payload->get_length() + sizeof(channel_id) + sizeof(uint16_t);

                        if (size_needed > size_left)
                        {
                            manager->remove_channel_for_client(new_cl_id, ch_id);

                            if (ch_id == CONTROL_CHANNEL_ID)
                            {
                                manager->remove_client(new_cl_id);
                                remove_client(new_cl_id);

                                return;
                            }
                        }
                        else
                        {
                            serialize_channel_setup_info(ch_id, res_setup_info.payload->get_buffer(), ack_pkt->get_buffer() + ack_payload_offset, ack_pkt->get_length());
                            ack_payload_offset += size_needed;
                            channel_cnt++;
                        }
                        // ℹ️  improvement :  coudl simply ask for size of setup info before hand beore adding channel
                    }

                    ack_pkt->set_length(ack_payload_offset);

                    //
                    uint32_t CHANNEL_CNT_OFFSET = CONTROL_HEADER_SIZE + COMMON_HEADER_SIZE;
                    uint16_t n_channel_cnt = htons(channel_cnt);
                    memcpy(ack_pkt->get_buffer() + CHANNEL_CNT_OFFSET, &n_channel_cnt, sizeof(uint16_t));

                    //

                    control_header ack_header;
                    ack_header.flags = (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SYN |
                                       (uint8_t)CONTROL_PACKET_HEADER_FLAGS::ACK |
                                       (uint8_t)CONTROL_PACKET_HEADER_FLAGS::CHN |
                                       (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SUM;
                    ack_header.reserved = 0;

                    serialize_control_header(ack_pkt->get_buffer() + COMMON_HEADER_SIZE, CONTROL_HEADER_SIZE, ack_header);

                    ack_header.reserved += calculate_checksum(ack_pkt->get_buffer(), ack_pkt->get_length()); // Payload part

                    serialize_control_header(ack_pkt->get_buffer() + COMMON_HEADER_SIZE, CONTROL_HEADER_SIZE, ack_header);

                    manager->send_control_packet_via_transport(new_cl_id, std::move(ack_pkt));
                }
            }
            else
            {
                if (!is_active_connection)
                    return;

                // Handle Established Control Packets (ACK, FIN, RST, KeepAlive)
                // ...
            }
        }

        void set_channel_manager(std::weak_ptr<i_session_control_callback> ch_manager)
        {
            data_channel_manager = ch_manager;
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

enum class CONTROL_PACKET_HEADER_FLAGS : uint8_t
{
    SYN = 1,
    ACK = (1 << 1),
    RST = (1 << 2),
    FIN = (1 << 3),
    SUM = (1 << 4), // reserved bits are checksum
    CHN = (1 << 5)  // has channels info
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
        if (rcvd_flags & (uint8_t)CONTROL_PACKET_HEADER_FLAGS::RST)
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

            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SYN)
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
            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::RST)
            {
                current_state = CONNECTION_STATE::CLOSED;
            }
            else if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::ACK)
            {
                current_state = CONNECTION_STATE::ESTABLISHED;
            }
            else
                send_rst();
            break;
        }
        case CONNECTION_STATE::ESTABLISHED:
        {
            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::FIN)
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
            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::ACK)
            {
                current_state = CONNECTION_STATE::CLOSED;
            }
            else
                send_rst();

            break;
        }
        case CONNECTION_STATE::FIN_WAIT1:
        {
            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::ACK)
            {
                current_state = CONNECTION_STATE::FIN_WAIT2;
            }
            else if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::FIN)
            {
                send_ack();
                current_state = CONNECTION_STATE::CLOSING;
            }
            else if (rcvd_flags == ((uint8_t)CONTROL_PACKET_HEADER_FLAGS::FIN | (uint8_t)CONTROL_PACKET_HEADER_FLAGS::ACK))
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
            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::FIN)
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
            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::ACK)
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