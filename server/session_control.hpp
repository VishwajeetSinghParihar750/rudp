#pragma once

#include <unordered_map>
#include <memory>

#include "types.hpp"
#include "rudp_protocol_packet.hpp"
#include "i_udp_callback.hpp"
#include "i_session_control_callback.hpp"
#include "i_channel_manager_callback.hpp"
#include "transport_addr.hpp"

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
        return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS ::ACK);
    }

    uint8_t get_rst_flag() const
    {
        return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::RST);
    }

    uint8_t get_fin_flag() const
    {
        return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::FIN);
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
            if (rcvd_flags == static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::SYN))
            {
                current_state = CONNECTION_STATE::SYN_RCVD;
                return {get_ack_flag(), false};
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

class session_control : public i_udp_callback, public i_channel_manager_callback
{

    std::shared_ptr<i_udp_callback> udp_ptr;
    std::weak_ptr<i_session_control_callback> channel_manager_ptr;
    std::unordered_map<transport_addr, client_id, transport_addr_hasher> clients_addr_to_id;
    std::unordered_map<client_id, transport_addr> clients_id_to_addr;
    std::unordered_map<client_id, std::unique_ptr<connection_state_machine>> clients_fsm;

    void add_control_header(const client_id &cl_id, const rudp_protocol_packet &pkt)
    {
    }

    bool verify_can_send_data(const client_id &cl_id) {}

    void on_transport_send_control_packet()
    {
    }

    void parse_session_control_packet_header(const rudp_protocol_packet &incoming_pkt)
    {

        // set current header
    }

    bool verify_can_receive_data() {}

    void send_incoming_packet_to_channel_mananger(std::unique_ptr<rudp_protocol_packet> incoming_pkt)
    {
    }

public:
    void set_udp(std::shared_ptr<i_udp_callback> udp_ptr_)
    {
        udp_ptr = udp_ptr_;
    }

    void set_channel_manager(std::weak_ptr<i_session_control_callback> channel_manager_)
    {
        channel_manager_ptr = channel_manager_;
    }

    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt, std::unique_ptr<transport_addr> source_addr) override
    {
        parse_session_control_packet_header(*pkt);

        if (verify_can_receive_data())
        {
            auto it = clients_addr_to_id.find(*source_addr);

            if (it != clients_addr_to_id.end())
            {
                if (auto cm_sp = channel_manager_ptr.lock())
                {
                    cm_sp->on_transport_receive(it->second, std::move(pkt));
                }
            }
        }
    }

    void on_transport_send_data(const client_id &cl_id, std::unique_ptr<rudp_protocol_packet> pkt) override
    {

        if (verify_can_send_data(cl_id))
        {
            add_control_header(cl_id, *pkt);
            udp_ptr->send_packet_to_network(clients_id_to_addr[cl_id], pkt->get_buffer(), pkt->get_length());
        }
    }
};