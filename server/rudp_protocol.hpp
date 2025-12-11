#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>
#include <cassert>
#include <thread>
#include <arpa/inet.h>

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

    constexpr size_t CONTROL_HEADER_SIZE = 4;

    struct control_header
    {
        uint16_t total_len;
        uint8_t flags;
        // syn, ack, fin, rst, sum = reserved bits are checksum , cnl = has channel info in payload
        uint8_t reserved;

        control_header() = default;
        control_header(uint8_t f) : total_len(CONTROL_HEADER_SIZE), flags(f), reserved(0) {}
    };

    class session_control
    {
        std::weak_ptr<i_session_control_callback> data_channel_manager;
        std::unordered_map<client_id, connection_state_machine> clients_state_machine;
        std::unordered_map<client_id, std::deque<char>> client_stream_buffers;

        static void serialize_control_header(char *buf, const size_t &len, const control_header &ss_header)
        {
            assert(len >= CONTROL_HEADER_SIZE);

            // Serialize Total Length
            uint16_t n_len = htons(ss_header.total_len);
            memcpy(buf, &n_len, sizeof(uint16_t));

            // Serialize Flags and Reserved
            memcpy(buf + sizeof(uint16_t), &ss_header.flags, sizeof(uint8_t));
            memcpy(buf + sizeof(uint16_t) + sizeof(uint8_t), &ss_header.reserved, sizeof(uint8_t));
        }

        static control_header deserialize_control_header(const char *buf, const uint32_t &len)
        {
            assert(len >= CONTROL_HEADER_SIZE);
            control_header ss_header;

            // Deserialize Total Length
            memcpy(&ss_header.total_len, buf, sizeof(uint16_t));
            ss_header.total_len = ntohs(ss_header.total_len);

            // Deserialize Flags and Reserved
            memcpy(&ss_header.flags, buf + sizeof(uint16_t), sizeof(uint8_t));
            memcpy(&ss_header.reserved, buf + sizeof(uint16_t) + sizeof(uint8_t), sizeof(uint8_t));

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
                if (cl_id != INVALID_CLIENT_ID && !clients_state_machine.contains(cl_id))
                    return cl_id;
            }
        }

        static std::vector<channel_setup_info> deserialize_channel_setup_info(const char *buf, const uint32_t &len)
        {
            // it shoudl start with 16 bits channel count, then it should have [ chanel id , len, payload ] ...

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
            clients_state_machine.emplace(cl_id);
            client_stream_buffers.emplace();
        }
        void remove_client(const client_id &cl_id)
        {
            clients_state_machine.erase(cl_id);
            client_stream_buffers.erase(cl_id);
        }

        std::jthread input_loop_thread;
        void input_event_loop(std::stop_token token)
        {
            std::unique_ptr<raw_packet> ibuf = std::make_unique<raw_packet>(1500);

            channel_id ch_id;
            client_id cl_id;

            while (!token.stop_requested())
            {
                auto sp = data_channel_manager.lock();
                if (sp == nullptr)
                    return;

                ssize_t bytes_read = sp->read_from_control_channel_blocking(
                    ch_id,
                    cl_id,
                    ibuf->get_buffer(),
                    ibuf->get_capacity());

                if (bytes_read <= 0 || ch_id != CONTROL_CHANNEL_ID)
                {
                    continue;
                }

                std::deque<char> &client_buffer = client_stream_buffers[cl_id];

                client_buffer.insert(client_buffer.end(),
                                     ibuf->get_const_buffer(),
                                     ibuf->get_const_buffer() + bytes_read);

                while (client_buffer.size() >= CONTROL_HEADER_SIZE)
                {
                    uint16_t net_total_len;

                    char temp_len_bytes[sizeof(uint16_t)];
                    temp_len_bytes[0] = client_buffer[0];
                    temp_len_bytes[1] = client_buffer[1];
                    memcpy(&net_total_len, temp_len_bytes, sizeof(uint16_t));

                    uint16_t total_packet_size = ntohs(net_total_len);

                    if (client_buffer.size() < total_packet_size)
                    {
                        break; // wait for more data.
                    }

                    std::unique_ptr<raw_packet> complete_packet = std::make_unique<raw_packet>(total_packet_size);

                    for (size_t i = 0; i < total_packet_size; ++i)
                    {
                        complete_packet->get_buffer()[i] = client_buffer[i];
                    }
                    complete_packet->set_length(total_packet_size);

                    handle_control_packet(cl_id, true, nullptr,
                                          complete_packet->get_buffer(),
                                          complete_packet->get_length());

                    client_buffer.erase(client_buffer.begin(), client_buffer.begin() + total_packet_size);
                }
            }
        }

    public:
        void handle_control_packet(const client_id &cl_id, bool is_active_connection, std::unique_ptr<i_sockaddr> source_addr, const char *ibuf, const uint32_t &sz)
        {
            if (sz < CONTROL_HEADER_SIZE)
                return;

            control_header cur_header = deserialize_control_header(ibuf, sz);

            // If the buffer size is less than what the header claims, the packet is incomplete/corrupt.
            if (sz < cur_header.total_len)
                return;

            const char *payload_ptr = ibuf + CONTROL_HEADER_SIZE;
            // Use total_len from header to determine payload size
            uint32_t payload_len = cur_header.total_len - CONTROL_HEADER_SIZE;

            client_id target_cl_id = cl_id;
            connection_state_machine *fsm = nullptr;

            if (target_cl_id == INVALID_CLIENT_ID && !is_active_connection)
            {
                if (!(cur_header.flags & (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SYN))
                {
                    return;
                }

                auto manager = data_channel_manager.lock();
                if (!manager)
                    return;

                target_cl_id = get_new_client_id();
                add_client(target_cl_id);
                manager->add_client(target_cl_id, *source_addr);

                fsm = &clients_state_machine.at(target_cl_id);
            }
            else if (is_active_connection && clients_state_machine.contains(target_cl_id))
            {
                fsm = &clients_state_machine.at(target_cl_id);
            }
            else
            {
                return;
            }

            uint8_t final_flags = 0;
            if (cur_header.flags & (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SUM)
            {
                // We use cur_header.total_len to be consistent with the packet definition
                uint8_t actual_checksum = calculate_checksum(ibuf, cur_header.total_len);
                if (actual_checksum != (~(uint8_t(0))))
                {
                    return;
                }
                final_flags |= (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SUM;
            }

            //
            uint8_t fsm_flags_mask = (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SYN |
                                     (uint8_t)CONTROL_PACKET_HEADER_FLAGS::ACK |
                                     (uint8_t)CONTROL_PACKET_HEADER_FLAGS::RST |
                                     (uint8_t)CONTROL_PACKET_HEADER_FLAGS::FIN;

            uint8_t rcvd_fsm_flags = cur_header.flags & fsm_flags_mask;

            connection_state_machine::fsm_result result = fsm->handle_change(rcvd_fsm_flags);

            if (result.close_connection)
            {
                auto manager = data_channel_manager.lock();
                if (manager)
                {
                    manager->remove_client(target_cl_id);
                }
                remove_client(target_cl_id);
                return; // Connection closed, no response needed.
            }

            final_flags |= result.response_flags;

            bool needs_chn_response = (cur_header.flags & (uint8_t)CONTROL_PACKET_HEADER_FLAGS::CHN) && (final_flags != 0);

            if (needs_chn_response)
                final_flags |= uint8_t(CONTROL_PACKET_HEADER_FLAGS::CHN);

            if (final_flags == 0)
                return; // nothing to send back

            //
            std::unique_ptr<raw_packet> res_pkt = std::make_unique<raw_packet>(1500);
            uint32_t current_offset = COMMON_HEADER_SIZE + CONTROL_HEADER_SIZE;
            uint32_t control_payload_start = current_offset;

            if (needs_chn_response)
            {
                auto manager = data_channel_manager.lock();
                if (!manager)
                    return;

                std::vector<channel_setup_info> received_setup_info = deserialize_channel_setup_info(payload_ptr, payload_len);

                uint32_t channel_data_offset = current_offset + sizeof(uint16_t);
                uint16_t channel_cnt = 0;

                for (auto &i : received_setup_info)
                {
                    channel_id ch_id = i.ch_id;
                    channel_setup_info res_setup_info;

                    manager->add_channel_for_client(target_cl_id, ch_id);
                    manager->process_channel_setup_request(target_cl_id, std::move(i));
                    res_setup_info = manager->get_channel_setup_info(target_cl_id, ch_id);

                    uint32_t size_needed = res_setup_info.payload->get_length() + sizeof(channel_id) + sizeof(uint16_t);
                    uint32_t size_left = 1500 - channel_data_offset;

                    if (size_needed > size_left)
                    {
                        manager->remove_channel_for_client(target_cl_id, ch_id);

                        if (ch_id == CONTROL_CHANNEL_ID)
                        {
                            manager->remove_client(target_cl_id);
                            remove_client(target_cl_id);
                            return;
                        }
                    }
                    else
                    {
                        serialize_channel_setup_info(ch_id, res_setup_info.payload->get_buffer(), res_pkt->get_buffer() + channel_data_offset, res_setup_info.payload->get_length());
                        channel_data_offset += size_needed;
                        channel_cnt++;
                    }
                }

                uint16_t n_channel_cnt = htons(channel_cnt);
                memcpy(res_pkt->get_buffer() + current_offset, &n_channel_cnt, sizeof(uint16_t));

                current_offset = channel_data_offset;
            }

            res_pkt->set_length(current_offset);

            control_header res_header;
            res_header.flags = final_flags;

            // Calculate size of Control Header + Control Payload (exclude Common Header)
            res_header.total_len = (uint16_t)(current_offset - COMMON_HEADER_SIZE);

            serialize_control_header(res_pkt->get_buffer() + COMMON_HEADER_SIZE, CONTROL_HEADER_SIZE, res_header);

            if (final_flags & (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SUM)
            {
                uint8_t final_checksum = calculate_checksum(res_pkt->get_buffer(), res_pkt->get_length());
                res_header.reserved = (uint8_t)(~(final_checksum));
                serialize_control_header(res_pkt->get_buffer() + COMMON_HEADER_SIZE, CONTROL_HEADER_SIZE, res_header);
            }

            auto manager = data_channel_manager.lock();
            if (manager)
            {
                if (is_active_connection)
                    manager->write_to_channel(CONTROL_CHANNEL_ID, target_cl_id, res_pkt->get_buffer(), res_pkt->get_length());
                else
                    manager->send_control_packet_via_transport(target_cl_id, std::move(res_pkt));
            }
        }

        void setup_control_session(std::weak_ptr<i_session_control_callback> ch_manager)
        {
            data_channel_manager = ch_manager;
            auto sp = data_channel_manager.lock();
            assert(sp != nullptr);

            sp->add_control_channel(channel_type::RELIABLE_ORDERED_CHANNEL);

            input_loop_thread = std::jthread([&](std::stop_token stoken)
                                             { input_event_loop(std::move(stoken)); });
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

struct connection_state_machine
{
    CONNECTION_STATE current_state = CONNECTION_STATE::LISTEN;

    struct fsm_result
    {
        uint8_t response_flags = 0;
        bool close_connection = false;
    };

    uint8_t get_ack_flag() const
    {
        return (uint8_t)CONTROL_PACKET_HEADER_FLAGS ::ACK;
    }

    uint8_t get_rst_flag() const
    {
        return (uint8_t)CONTROL_PACKET_HEADER_FLAGS::RST;
    }

    uint8_t get_fin_flag() const
    {
        return (uint8_t)CONTROL_PACKET_HEADER_FLAGS::FIN;
    }

    fsm_result close()
    {
        if (current_state == CONNECTION_STATE::SYN_RCVD)
        {
            current_state = CONNECTION_STATE::CLOSED;
            return {get_rst_flag(), true};
        }
        else if (current_state == CONNECTION_STATE::ESTABLISHED)
        {
            current_state = CONNECTION_STATE::FIN_WAIT1;
            return {get_fin_flag(), false};
        }
        else if (current_state == CONNECTION_STATE::CLOSE_WAIT)
        {
            current_state = CONNECTION_STATE::LAST_ACK;
            return {get_fin_flag(), false};
        }
        return {0, false};
    }

    fsm_result handle_change(uint8_t rcvd_flags)
    {
        if (rcvd_flags & get_rst_flag())
        {
            if (current_state != CONNECTION_STATE::LISTEN && current_state != CONNECTION_STATE::CLOSED)
            {
                current_state = CONNECTION_STATE::CLOSED;
                return {0, true};
            }
            return {0, false};
        }

        switch (current_state)
        {
        case CONNECTION_STATE::LISTEN:
        {
            if (rcvd_flags == (uint8_t)CONTROL_PACKET_HEADER_FLAGS::SYN)
            {
                current_state = CONNECTION_STATE::SYN_RCVD;
                return {(uint8_t)CONTROL_PACKET_HEADER_FLAGS::SYN | get_ack_flag(), false};
            }
            else
            {
                return {get_rst_flag(), false};
            }
        }

        case CONNECTION_STATE::SYN_RCVD:
        {
            if (rcvd_flags & get_rst_flag())
            {
                current_state = CONNECTION_STATE::CLOSED;
                return {0, true};
            }
            else if (rcvd_flags == get_ack_flag())
            {
                current_state = CONNECTION_STATE::ESTABLISHED;
                return {0, false};
            }
            else
            {
                return {get_rst_flag(), false};
            }
        }
        case CONNECTION_STATE::ESTABLISHED:
        {
            if (rcvd_flags == get_fin_flag())
            {
                current_state = CONNECTION_STATE::CLOSE_WAIT;
                return {get_ack_flag(), false};
            }
            return {0, false};
        }
        case CONNECTION_STATE::LAST_ACK:
        {
            if (rcvd_flags == get_ack_flag())
            {
                current_state = CONNECTION_STATE::CLOSED;
                return {0, true};
            }
            return {0, false};
        }
        case CONNECTION_STATE::FIN_WAIT1:
        {
            if (rcvd_flags == get_ack_flag())
            {
                current_state = CONNECTION_STATE::FIN_WAIT2;
                return {0, false};
            }
            else if (rcvd_flags == get_fin_flag())
            {
                current_state = CONNECTION_STATE::CLOSING;
                return {get_ack_flag(), false};
            }
            else if (rcvd_flags == (get_fin_flag() | get_ack_flag()))
            {
                current_state = CONNECTION_STATE::TIME_WAIT;
                return {get_ack_flag(), false};
            }
            return {0, false};
        }
        case CONNECTION_STATE::FIN_WAIT2:
        {
            if (rcvd_flags == get_fin_flag())
            {
                current_state = CONNECTION_STATE::TIME_WAIT;
                return {get_ack_flag(), false};
            }
            return {0, false};
        }
        case CONNECTION_STATE::CLOSING:
        {
            if (rcvd_flags == get_ack_flag())
            {
                current_state = CONNECTION_STATE::TIME_WAIT;
                return {0, false};
            }
            return {0, false};
        }
        default:
            return {0, false};
        }
    }
};