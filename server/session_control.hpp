#pragma once

#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

#include "../common/rudp_protocol_packet.hpp"
#include "i_udp_for_session_control.hpp"
#include "i_session_control_for_channel_manager.hpp"
#include "i_session_control_for_udp.hpp"
#include "i_channel_manager_for_session_control.hpp"

#include "transport_addr.hpp"
#include "rudp_protocol.hpp"
#include "timer_manager.hpp"
#include "../common/thread_safe_unordered_map.hpp"
#include "types.hpp"

class i_server;

enum class CONNECTION_STATE
{
    CLOSED,
    LISTEN,
    ESTABLISHED,
    CLOSE_WAIT,
    LAST_ACK,
    TIME_WAIT
};
enum class CONTROL_PACKET_HEADER_FLAGS : uint8_t
{
    SYN = 1,
    ACK = (1 << 1),
    RST = (1 << 2),
    FIN = (1 << 3),
};

struct connection_state_machine : public std::enable_shared_from_this<connection_state_machine>
{
    std::recursive_mutex g_connection_state_machine_mutex;

    static constexpr uint32_t ROUND_TRIP_TIME = 2000; // ℹ️ NEED TO KNOWx`

    std::atomic<CONNECTION_STATE> current_state = CONNECTION_STATE::LISTEN;

    std::function<void(uint8_t)> on_send_control_packet_to_transport;
    std::shared_ptr<timer_manager> global_timer_manager;

    connection_state_machine(std::function<void(uint8_t)> f, std::shared_ptr<timer_manager> timer_man)
        : current_state(CONNECTION_STATE::LISTEN), on_send_control_packet_to_transport(f), global_timer_manager(timer_man) {}
    ~connection_state_machine()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        if (current_state.load() != CONNECTION_STATE::CLOSED)
        {
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();
        }
    }

    struct fsm_result
    {
        bool close_connection = false;
        bool stop_data_exchange = false;
    };

    struct to_send_response
    {
        uint8_t response_flags = 0;
        bool to_send = false;
    };

    to_send_response last_response;
    TimerInfoTimePoint last_ack_send_time;

    bool can_exchange_data()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        return current_state.load() == CONNECTION_STATE::ESTABLISHED;
    }

    void set_last_ack_sent_time()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        last_ack_send_time = std::chrono::steady_clock::now();
    }

    to_send_response get_to_send_response()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        auto toret = last_response;

        if ((toret.response_flags & (get_ack_flag())) && toret.to_send)
            set_last_ack_sent_time();

        last_response.to_send = false;

        return toret;
    }

    void send_response_to_network_without_piggybacking()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        if (last_response.to_send)
        {
            if (last_response.response_flags & (get_ack_flag()))
                set_last_ack_sent_time();

            on_send_control_packet_to_transport(last_response.response_flags);
            last_response.to_send = false;
        }
    }

    uint8_t get_ack_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::ACK); }
    uint8_t get_rst_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::RST); }
    uint8_t get_fin_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::FIN); }
    uint8_t get_syn_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::SYN); }

    fsm_result close()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        if (current_state.load() == CONNECTION_STATE::ESTABLISHED)
        {
            current_state.store(CONNECTION_STATE::LAST_ACK);

            last_response = {get_fin_flag(), true};
            send_response_to_network_without_piggybacking();

            std::weak_ptr<connection_state_machine> self_weak_ptr = shared_from_this();

            std::function<void()> cb = [self_weak_ptr, cb, retries = 5]() mutable -> void
            {
                auto sp = self_weak_ptr.lock();
                if (sp == nullptr or --retries == 0)
                    return;

                std::lock_guard<std::recursive_mutex> lg(sp->g_connection_state_machine_mutex);
                if (sp->current_state.load() == CONNECTION_STATE::LAST_ACK)
                {
                    sp->last_response = {sp->get_fin_flag(), true};
                    sp->send_response_to_network_without_piggybacking();
                    sp->global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(ROUND_TRIP_TIME * 2), cb));
                }
            };

            global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(ROUND_TRIP_TIME * 2), cb));

            return {false, false};
        }

        else
        {
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();

            current_state.store(CONNECTION_STATE::CLOSED);
            return {true, false};
        }
    }

    fsm_result handle_change(uint8_t rcvd_flags)
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);

        if (current_state.load() == CONNECTION_STATE::CLOSED)
        {
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();
            return {false, false};
        }

        if (last_response.to_send)
        {
            send_response_to_network_without_piggybacking();
            return {false, false};
        }

        if (rcvd_flags & get_rst_flag())
        {
            current_state.store(CONNECTION_STATE::CLOSED);
            return {true, true};
        }

        switch (current_state.load())
        {
        case CONNECTION_STATE::LISTEN:
            if (rcvd_flags == get_syn_flag())
            {
                current_state.store(CONNECTION_STATE::ESTABLISHED);
                last_response = {get_ack_flag(), true};
            }
            break;

        case CONNECTION_STATE::ESTABLISHED:
            if (rcvd_flags == get_fin_flag())
            {
                current_state.store(CONNECTION_STATE::TIME_WAIT);

                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking();

                std::weak_ptr<connection_state_machine> this_weak_ptr = shared_from_this();

                std::function<void()> cb = [this_weak_ptr, cb, retries = 5]() mutable -> void
                {
                    auto sp = this_weak_ptr.lock();
                    if (sp == nullptr or --retries == 0)
                        return;

                    std::lock_guard<std::recursive_mutex> lg(sp->g_connection_state_machine_mutex);

                    duration_ms time_spent = std::chrono::duration_cast<duration_ms>(std::chrono::steady_clock::now() - sp->last_ack_send_time);
                    if (time_spent < duration_ms(connection_state_machine::ROUND_TRIP_TIME * 2))
                    {
                        sp->last_response = {sp->get_ack_flag(), true};
                    }

                    if (time_spent < duration_ms(connection_state_machine::ROUND_TRIP_TIME * 2))
                    {
                        sp->send_response_to_network_without_piggybacking();
                        sp->global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(connection_state_machine::ROUND_TRIP_TIME * 2), cb));
                    }
                    else
                    {
                        sp->current_state.store(CONNECTION_STATE::CLOSED);
                    }
                };

                global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(connection_state_machine::ROUND_TRIP_TIME * 2), cb));

                return {false, true};
            }
            else if (rcvd_flags == get_syn_flag())
            {
                // means ack got lost for clients syn
                last_response = {get_ack_flag(), true};
            }

            break;

        case CONNECTION_STATE::TIME_WAIT:
            if (rcvd_flags == get_fin_flag())
            {
                // means ack got lost for fin
                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking();
            }
            break;

        case CONNECTION_STATE::LAST_ACK:
            if (rcvd_flags == get_ack_flag())
            {
                current_state.store(CONNECTION_STATE::CLOSED);
                return {true, false};
            }
            break;

        default:
            break;
        }

        return {false, false};
    }
};

class session_control : public i_session_control_for_udp, public i_session_control_for_channel_manager, public std::enable_shared_from_this<session_control>
{
    std::atomic<bool> server_closed = false;

    std::shared_ptr<timer_manager> global_timer_manager;
    std::shared_ptr<i_udp_for_session_control> udp_ptr;
    std::weak_ptr<i_channel_manager_for_session_control> channel_manager_ptr;
    thread_safe_unordered_map<transport_addr, client_id, transport_addr_hasher> clients_addr_to_id;
    thread_safe_unordered_map<client_id, transport_addr> clients_id_to_addr;
    thread_safe_unordered_map<client_id, std::shared_ptr<connection_state_machine>> clients_fsm;

    thread_safe_unordered_map<client_id, std::shared_ptr<std::atomic<int>>> teardown_counter;

    void perform_final_cleanup(client_id cl_id)
    {
        auto addr_opt = clients_id_to_addr.get(cl_id);
        if (addr_opt)
        {
            clients_addr_to_id.erase(addr_opt.value());
            clients_id_to_addr.erase(cl_id);
        }
        teardown_counter.erase(cl_id);
        clients_fsm.erase(cl_id);
    }
    void trigger_teardown_step(client_id cl_id)
    {
        auto counter = teardown_counter.get(cl_id);
        if (!counter)
            return;

        int previous_value = (*counter)->fetch_add(1);

        if (previous_value == 1)
        {
            perform_final_cleanup(cl_id);
        }
    }

    void add_session_control_header(const client_id &cl_id, rudp_protocol_packet &pkt)
    {
        auto fsm_opt = clients_fsm.get(cl_id);
        if (!fsm_opt)
            return;
        connection_state_machine::to_send_response res = fsm_opt.value()->get_to_send_response();
        rudp_protocol::session_control_header header;
        header.flags = 0, header.reserved = 0;

        if (res.to_send)
            header.flags |= res.response_flags;

        header.reserved = htonl(header.reserved);
        //
        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET, &header.flags, sizeof(header.flags));
        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(header.flags), &header.reserved, sizeof(header.reserved));
    }

    bool verify_can_exchange_data(const client_id &cl_id)
    {
        if (server_closed.load())
            return false;
        auto fsm_opt = clients_fsm.get(cl_id);
        return fsm_opt && fsm_opt.value()->can_exchange_data();
    }

    void parse_session_control_packet_header(const rudp_protocol_packet &incoming_pkt, const transport_addr &source_addr)
    {
        uint8_t flags = *reinterpret_cast<uint8_t *>(incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET);
        uint32_t reserved = *reinterpret_cast<uint8_t *>(incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(flags));
        reserved = ntohl(reserved);

        if (!clients_addr_to_id.contains(source_addr))
        {
            if (server_closed.load())
                return; // server closed

            client_id cl_id;
            while (true)
            {
                cl_id = get_random_client_id();
                if (!clients_id_to_addr.contains(cl_id) && !(cl_id == INVALID_CLIENT_ID))
                    break;
            }
            create_fsm_for_client(cl_id, source_addr);
            auto fsm_opt = clients_fsm.get(cl_id);
            if (!fsm_opt)
                return;

            connection_state_machine::fsm_result res = fsm_opt.value()->handle_change(flags & ((uint8_t(1) << 4) - 1));

            if (res.close_connection)
                clients_fsm.erase(cl_id);
            else
            {
                clients_addr_to_id.insert(source_addr, cl_id);
                clients_id_to_addr.insert(cl_id, source_addr);
            }
        }
        else
        {
            auto cid_opt = clients_addr_to_id.get(source_addr);
            if (!cid_opt)
                return;
            client_id cl_id = cid_opt.value();

            auto fsm_opt = clients_fsm.get(cl_id);
            if (!fsm_opt)
                return;

            connection_state_machine::fsm_result res = fsm_opt.value()->handle_change(flags & ((uint8_t(1) << 4) - 1));

            if (res.close_connection)
            {
                if (!teardown_counter.contains(cl_id))
                    teardown_counter.insert(cl_id, std::make_shared<std::atomic<int>>(0));

                trigger_teardown_step(cl_id);
            }

            if (res.stop_data_exchange)
            {
                if (auto sp = channel_manager_ptr.lock())
                {
                    if (!teardown_counter.contains(cl_id))
                        teardown_counter.insert(cl_id, std::make_shared<std::atomic<int>>(0));

                    sp->remove_client(cl_id);
                }
            }
        }
    }

    void create_fsm_for_client(const client_id &cl_id, const transport_addr &source_addr)
    {
        clients_fsm.insert(cl_id, std::make_shared<connection_state_machine>([source_addr, this](uint8_t fsm_flags)
                                                                             {
                                                                                  char buf[rudp_protocol::SESSION_CONTROL_HEADER_SIZE];

                                                                                  memcpy(buf, &fsm_flags, sizeof(fsm_flags));
                                                                                  // here we culd have more of stuff as session control if we wanna add 

                                                                                  this->udp_ptr->send_packet_to_network(source_addr, buf, sizeof(buf)); }, global_timer_manager));
    }

public:
    // selective access
    class server_setup_access_key
    {
        friend std::shared_ptr<i_server> create_server(const char *);

    private:
        server_setup_access_key() {}
    };

    void set_timer_manager(std::shared_ptr<timer_manager> timer_man, server_setup_access_key) { global_timer_manager = timer_man; }
    void set_udp(std::shared_ptr<i_udp_for_session_control> udp_ptr_, server_setup_access_key)
    {
        udp_ptr = udp_ptr_;
    }
    void set_channel_manager(std::weak_ptr<i_channel_manager_for_session_control> channel_manager_, server_setup_access_key)
    {
        channel_manager_ptr = channel_manager_;
    }

    void on_close_server() override
    {
        server_closed.store(true);

        std::vector<client_id> to_clean_up;
        auto items = clients_fsm.items();
        for (auto &p : items)
        {
            client_id cl_id = p.first;
            auto fsm = p.second;
            connection_state_machine::fsm_result res = fsm->close();
            if (res.close_connection)
                to_clean_up.push_back(cl_id); // directly closing on netowrk and applicaiton togehter
            else
                teardown_counter.insert(cl_id, std::make_shared<std::atomic<int>>(1)); // application side has said close, watiting for network now
        }
        for (auto i : to_clean_up)
            perform_final_cleanup(i);

        if (clients_fsm.size() > 0)
        {
            auto this_shared_ptr = shared_from_this();
            std::function<void()> cb = [this_shared_ptr, cb]
            {
                if (this_shared_ptr->clients_fsm.size() > 0)
                {
                    this_shared_ptr->global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(100), cb));
                }
            };

            global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(100), cb));
        } // this shared ptr will keep session control alive until all fsms gracefully close
    }
    void notify_removal_of_client(const client_id &cl_id) override
    {
        trigger_teardown_step(cl_id);
    }

    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt, std::unique_ptr<transport_addr> source_addr) override
    {
        parse_session_control_packet_header(*pkt, *source_addr);

        auto cid_opt = clients_addr_to_id.get(*source_addr);
        if (cid_opt && verify_can_exchange_data(cid_opt.value()))
            if (auto cm_sp = channel_manager_ptr.lock())
                cm_sp->on_transport_receive(cid_opt.value(), std::move(pkt));
    }

    void on_transport_send_data(const client_id &cl_id, std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (verify_can_exchange_data(cl_id))
        {
            add_session_control_header(cl_id, *pkt);
            auto addr_opt = clients_id_to_addr.get(cl_id);
            if (addr_opt)
                udp_ptr->send_packet_to_network(addr_opt.value(), pkt->get_buffer(), pkt->get_length());
        }
    }
};