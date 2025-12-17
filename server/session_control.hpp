#pragma once

#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>

#include "../common/transport_addr.hpp"
#include "../common/timer_manager.hpp"
#include "../common/timer_info.hpp"
#include "../common/thread_safe_unordered_map.hpp"
#include "../common/rudp_protocol_packet.hpp"
#include "../common/logger.hpp"
#include "../common/types.hpp"

#include "i_udp_for_session_control.hpp"
#include "i_session_control_for_channel_manager.hpp"
#include "i_session_control_for_udp.hpp"
#include "i_channel_manager_for_session_control.hpp"

struct session_control_header
{
    uint8_t flags;
    uint32_t reserved;
};

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
inline std::string state_to_string(CONNECTION_STATE s)
{
    switch (s)
    {
    case CONNECTION_STATE::CLOSED:
        return "CLOSED";
    case CONNECTION_STATE::LISTEN:
        return "LISTEN";
    case CONNECTION_STATE::ESTABLISHED:
        return "ESTABLISHED";
    case CONNECTION_STATE::CLOSE_WAIT:
        return "CLOSE_WAIT";
    case CONNECTION_STATE::LAST_ACK:
        return "LAST_ACK";
    case CONNECTION_STATE::TIME_WAIT:
        return "TIME_WAIT";
    default:
        return "UNKNOWN";
    }
}

enum class CONTROL_PACKET_HEADER_FLAGS : uint8_t
{
    SYN = 1,
    ACK = (1 << 1),
    RST = (1 << 2),
    FIN = (1 << 3),
};
inline std::string flags_to_string(uint8_t flags)
{
    std::string s;
    if (flags & 1)
        s += "SYN|";
    if (flags & (1 << 1))
        s += "ACK|";
    if (flags & (1 << 2))
        s += "RST|";
    if (flags & (1 << 3))
        s += "FIN|";
    if (!s.empty())
        s.pop_back();
    return s.empty() ? "None" : s;
}

struct connection_state_machine : public std::enable_shared_from_this<connection_state_machine>
{
    std::recursive_mutex g_connection_state_machine_mutex;

    static constexpr uint32_t ROUND_TRIP_TIME = 2000;

    std::atomic<CONNECTION_STATE> current_state = CONNECTION_STATE::LISTEN;

    std::function<void(uint8_t)> on_send_control_packet_to_transport;
    std::shared_ptr<timer_manager> global_timer_manager;

    connection_state_machine(std::function<void(uint8_t)> f, std::shared_ptr<timer_manager> timer_man)
        : current_state(CONNECTION_STATE::LISTEN), on_send_control_packet_to_transport(f), global_timer_manager(timer_man)
    {
        logger::getInstance().logInfo("FSM created, state: LISTEN");
    }
    ~connection_state_machine()
    {
        // std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        // if (current_state.load() != CONNECTION_STATE::CLOSED)
        // {
        //     logger::getInstance().logWarning("FSM destructor called while not CLOSED. Sending RST.");
        //     last_response = {get_rst_flag(), true};
        //     send_response_to_network_without_piggybacking();
        //     current_state.store(CONNECTION_STATE::CLOSED);
        // }
        logger::getInstance().logInfo("FSM destroyed.");
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
        {
            set_last_ack_sent_time();
            logger::getInstance().logTest("FSM piggybacking " + flags_to_string(toret.response_flags) + " on data packet.");
        }

        last_response.to_send = false;

        return toret;
    }

    void send_response_to_network_without_piggybacking()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        if (last_response.to_send)
        {
            logger::getInstance().logTest("FSM sending control packet: " + flags_to_string(last_response.response_flags) + " (Standalone)");

            if (last_response.response_flags & (get_ack_flag()))
                set_last_ack_sent_time();

            on_send_control_packet_to_transport(last_response.response_flags);
            last_response.to_send = false;
        }
    }

    void last_ack_cb_with_retry(int retries_left = 5)
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);

        if (current_state.load() != CONNECTION_STATE::LAST_ACK || retries_left <= 0)
            return;

        logger::getInstance().logInfo("LAST_ACK retry " +
                                      std::to_string(6 - retries_left) + "/5");

        last_response = {get_fin_flag(), true};
        send_response_to_network_without_piggybacking();

        if (retries_left > 1)
        {
            std::weak_ptr<connection_state_machine> self_weak_ptr = shared_from_this();

            global_timer_manager->add_timer(std::make_unique<timer_info>(
                duration_ms(ROUND_TRIP_TIME * 2),
                [self_weak_ptr, retries_left]()
                {
                    if (auto sp = self_weak_ptr.lock())
                    {
                        sp->last_ack_cb_with_retry(retries_left - 1);
                    }
                }));
        }
        else
        {
            logger::getInstance().logWarning("LAST_ACK retries exhausted");
        }
    }

    void time_wait_cb_with_retry(int retries_left = 5)
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);

        duration_ms time_spent = std::chrono::duration_cast<duration_ms>(std::chrono::steady_clock::now() - last_ack_send_time);

        if (retries_left > 0 && time_spent < duration_ms(connection_state_machine::ROUND_TRIP_TIME * 2))
        {
            std::weak_ptr<connection_state_machine> self_weak_ptr = shared_from_this();

            global_timer_manager->add_timer(std::make_unique<timer_info>(
                duration_ms(ROUND_TRIP_TIME * 2),
                [self_weak_ptr, retries_left]()
                {
                    if (auto sp = self_weak_ptr.lock())
                    {
                        sp->time_wait_cb_with_retry(retries_left - 1);
                    }
                }));
        }
        else
        {
            current_state.store(CONNECTION_STATE::CLOSED);
            logger::getInstance().logInfo("Transition: TIME_WAIT -> CLOSED (2*RTT elapsed).");
        }
    }

    uint8_t get_ack_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::ACK); }
    uint8_t get_rst_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::RST); }
    uint8_t get_fin_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::FIN); }
    uint8_t get_syn_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::SYN); }

    fsm_result close()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        logger::getInstance().logInfo("FSM close() called from state: " + state_to_string(current_state.load()));

        if (current_state.load() == CONNECTION_STATE::ESTABLISHED)
        {
            current_state.store(CONNECTION_STATE::LAST_ACK);
            logger::getInstance().logInfo("FSM state transition: ESTABLISHED -> LAST_ACK. Sending FIN.");

            last_ack_cb_with_retry();

            return {false, false};
        }

        else
        {
            logger::getInstance().logError("FSM close() called from non-ESTABLISHED state. Sending RST. State: " + state_to_string(current_state.load()));
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();

            current_state.store(CONNECTION_STATE::CLOSED);
            return {true, false};
        }
    }

    fsm_result handle_change(uint8_t rcvd_flags)
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        logger::getInstance().logTest("FSM handle_change. Current: " + state_to_string(current_state.load()) + ", Received: " + flags_to_string(rcvd_flags));

        if (current_state.load() == CONNECTION_STATE::CLOSED)
        {
            logger::getInstance().logWarning("FSM received packet in CLOSED state. Sending RST.");
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();
            return {false, false};
        }

        if (last_response.to_send)
        {

            send_response_to_network_without_piggybacking();
        }

        if (rcvd_flags & get_rst_flag())
        {
            logger::getInstance().logError("FSM received RST. Transition to CLOSED.");
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
                logger::getInstance().logInfo("FSM state transition: LISTEN -> ESTABLISHED. Sending ACK.");
            }
            break;

        case CONNECTION_STATE::ESTABLISHED:
            if (rcvd_flags == get_fin_flag())
            {
                current_state.store(CONNECTION_STATE::TIME_WAIT);
                logger::getInstance().logInfo("FSM state transition: ESTABLISHED -> TIME_WAIT. Sending ACK and starting TIME_WAIT timer.");

                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking();

                time_wait_cb_with_retry();

                return {false, true};
            }
            else if (rcvd_flags == get_syn_flag())
            {

                last_response = {get_ack_flag(), true};
                logger::getInstance().logWarning("FSM received unexpected SYN in ESTABLISHED. Responding with ACK (Possible retransmission).");
            }

            break;

        case CONNECTION_STATE::TIME_WAIT:
            if (rcvd_flags == get_fin_flag())
            {

                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking();
                logger::getInstance().logWarning("FSM TIME_WAIT received retransmitted FIN. Resending ACK.");
            }
            break;

        case CONNECTION_STATE::LAST_ACK:
            if (rcvd_flags == get_ack_flag())
            {
                current_state.store(CONNECTION_STATE::CLOSED);
                logger::getInstance().logInfo("FSM state transition: LAST_ACK -> CLOSED. Received ACK for our FIN.");
                return {true, false};
            }
            break;

        default:
            logger::getInstance().logWarning("FSM: Unhandled flags " + flags_to_string(rcvd_flags) + " in state " + state_to_string(current_state.load()));
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
        logger::getInstance().logInfo("Session Control: Performing final cleanup for client " + std::to_string(cl_id));
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
        {
            logger::getInstance().logError("Session Control: Teardown triggered for client " + std::to_string(cl_id) + " but counter missing.");
            return;
        }

        int previous_value = (*counter)->fetch_add(1);
        logger::getInstance().logTest("Session Control: Teardown step triggered for client " + std::to_string(cl_id) + ". Counter: " + std::to_string(previous_value + 1));

        if (previous_value == 1)
        {
            logger::getInstance().logInfo("Session Control: Both Teardown flags set (Net+App). Final cleanup for client " + std::to_string(cl_id));
            perform_final_cleanup(cl_id);
        }
    }

    void add_session_control_header(const client_id &cl_id, rudp_protocol_packet &pkt)
    {
        auto fsm_opt = clients_fsm.get(cl_id);
        if (!fsm_opt)
        {
            logger::getInstance().logError("Session Control: Failed to add header, FSM missing for client " + std::to_string(cl_id));
            return;
        }
        connection_state_machine::to_send_response res = fsm_opt.value()->get_to_send_response();
        session_control_header header;
        header.flags = 0;
        header.reserved = 0;

        if (res.to_send)
        {
            header.flags |= res.response_flags;
            logger::getInstance().logTest("Session Control: Piggybacking flags " + flags_to_string(res.response_flags) + " on client " + std::to_string(cl_id) + " packet.");
        }

        uint32_t net_reserved = htonl(header.reserved);

        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET, &header.flags, sizeof(header.flags));
        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(header.flags), &net_reserved, sizeof(net_reserved));
    }

    bool verify_can_exchange_data(const client_id &cl_id)
    {
        if (server_closed.load())
        {
            logger::getInstance().logWarning("Session Control: Data exchange blocked (Server Closed).");
            return false;
        }
        auto fsm_opt = clients_fsm.get(cl_id);
        if (!fsm_opt)
        {
            logger::getInstance().logError("Session Control: Data exchange blocked, FSM missing for client " + std::to_string(cl_id));
            return false;
        }
        return fsm_opt.value()->can_exchange_data();
    }

    void parse_session_control_packet_header(const rudp_protocol_packet &incoming_pkt, const transport_addr &source_addr)
    {
        uint8_t flags = *reinterpret_cast<const uint8_t *>(incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET);

        uint32_t reserved_net;
        memcpy(&reserved_net, incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(flags), sizeof(reserved_net));
        uint32_t reserved = ntohl(reserved_net);

        uint8_t control_flags = flags & ((uint8_t(1) << 4) - 1);
        logger::getInstance().logTest("Session Control: Received packet from " + source_addr.to_string() + ". Flags: " + flags_to_string(control_flags));

        if (!clients_addr_to_id.contains(source_addr))
        {
            if (server_closed.load())
            {
                logger::getInstance().logWarning("Session Control: Ignoring connection attempt (Server Closed).");
                return;
            }

            client_id cl_id = INVALID_CLIENT_ID;

            do
            {
                cl_id = get_random_client_id();
            } while (clients_id_to_addr.contains(cl_id) || (cl_id == INVALID_CLIENT_ID));

            logger::getInstance().logInfo("Session Control: New client detected at " + source_addr.to_string() + ". Assigned ID: " + std::to_string(cl_id));

            teardown_counter.insert(cl_id, std::make_shared<std::atomic<int>>(0));
            create_fsm_for_client(cl_id, source_addr);

            auto fsm_opt = clients_fsm.get(cl_id);
            if (!fsm_opt)
            {
                logger::getInstance().logError("Session Control: FSM creation failed for new client " + std::to_string(cl_id));
                return;
            }

            connection_state_machine::fsm_result res = fsm_opt.value()->handle_change(control_flags);

            if (res.close_connection)
            {
                logger::getInstance().logWarning("Session Control: FSM immediately closed connection for new client. Erasing FSM/ID.");
                teardown_counter.erase(cl_id);
                clients_fsm.erase(cl_id);
            }
            else
            {
                clients_addr_to_id.insert(source_addr, cl_id);
                clients_id_to_addr.insert(cl_id, source_addr);

                if (fsm_opt.value()->can_exchange_data())
                {
                    if (auto cm_sp = channel_manager_ptr.lock())
                    {
                        cm_sp->add_client(cl_id);
                        logger::getInstance().logInfo("Session Control: New client " + std::to_string(cl_id) + " notified to Channel Manager.");
                    }
                }
            }
        }
        else
        {
            auto cid_opt = clients_addr_to_id.get(source_addr);
            if (!cid_opt)
            {
                logger::getInstance().logError("Session Control: Lookup failed for existing address " + source_addr.to_string());
                return;
            }
            client_id cl_id = cid_opt.value();

            auto fsm_opt = clients_fsm.get(cl_id);
            if (!fsm_opt)
            {
                logger::getInstance().logWarning("Session Control: Received packet from known address, but FSM missing for client " + std::to_string(cl_id) + ". Ignoring.");
                return;
            }

            connection_state_machine::fsm_result res = fsm_opt.value()->handle_change(control_flags);

            if (res.close_connection)
            {
                if (!teardown_counter.contains(cl_id))
                {
                    logger::getInstance().logError("Session Control: Missing counter during close_connection for client " + std::to_string(cl_id) + ". Created new counter.");
                }

                trigger_teardown_step(cl_id);
            }

            if (res.stop_data_exchange)
            {
                if (auto sp = channel_manager_ptr.lock())
                {
                    if (!teardown_counter.contains(cl_id))
                    {
                        logger::getInstance().logError("Session Control: Missing counter during stop_data_exchange for client " + std::to_string(cl_id) + ". Created new counter.");
                    }

                    sp->remove_client(cl_id);
                    logger::getInstance().logInfo("Session Control: Notifying Channel Manager of client removal for " + std::to_string(cl_id));
                }
            }
        }
    }

    void create_fsm_for_client(const client_id &cl_id, const transport_addr &source_addr)
    {
        clients_fsm.insert(cl_id, std::make_shared<connection_state_machine>([source_addr, cl_id, this](uint8_t fsm_flags)
                                                                             {
                                                                                char buf[rudp_protocol_packet::SESSION_CONTROL_HEADER_SIZE] = {0};

                                                                                uint32_t net_reserved = 0;
                                                                                
                                                                                
                                                                                memcpy(buf, &fsm_flags, sizeof(fsm_flags));
                                                                                
                                                                                memcpy(buf + sizeof(fsm_flags), &net_reserved, sizeof(net_reserved)); 
                                                                                
                                                                                logger::getInstance().logInfo("Control Packet Send: Client " + std::to_string(cl_id) + ", Flags: " + flags_to_string(fsm_flags));
                                                                                this->udp_ptr->send_packet_to_network(source_addr, buf, sizeof(buf)); }, global_timer_manager));
    }

public:
    class server_setup_access_key
    {
        friend std::shared_ptr<i_server> create_server(const char *);

    private:
        server_setup_access_key() {}
    };

    void set_timer_manager(std::shared_ptr<timer_manager> timer_man, server_setup_access_key)
    {
        global_timer_manager = timer_man;
        logger::getInstance().logInfo("Session Control: Timer Manager set.");
    }
    void set_udp(std::shared_ptr<i_udp_for_session_control> udp_ptr_, server_setup_access_key)
    {
        udp_ptr = udp_ptr_;
        logger::getInstance().logInfo("Session Control: UDP Transport set.");
    }
    void set_channel_manager(std::weak_ptr<i_channel_manager_for_session_control> channel_manager_, server_setup_access_key)
    {
        channel_manager_ptr = channel_manager_;
        logger::getInstance().logInfo("Session Control: Channel Manager set.");
    }

    void on_close_server() override
    {
        logger::getInstance().logCritical("Session Control: Server closure initiated.");
        server_closed.store(true);

        std::vector<client_id> to_clean_up;
        auto items = clients_fsm.items();
        for (auto &p : items)
        {
            client_id cl_id = p.first;
            auto fsm = p.second;
            connection_state_machine::fsm_result res = fsm->close();
            if (res.close_connection)
            {
                to_clean_up.push_back(cl_id);
                logger::getInstance().logInfo("Session Control: Client " + std::to_string(cl_id) + " FSM closed immediately. Preparing for cleanup.");
            }
            else
            {
                trigger_teardown_step(cl_id);
                logger::getInstance().logInfo("Session Control: Client " + std::to_string(cl_id) + " starting graceful teardown (Counter=1).");
            }
        }
        for (auto i : to_clean_up)
            perform_final_cleanup(i);

        if (clients_fsm.size() > 0)
        {
            logger::getInstance().logInfo("Session Control: Waiting for graceful FSM closures. Starting poll timer.");
            auto this_shared_ptr = shared_from_this();
            std::function<void()> cb = [this_shared_ptr, cb]
            {
                if (this_shared_ptr->clients_fsm.size() > 0)
                {
                    this_shared_ptr->global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(100), cb));
                }
                else
                {
                    logger::getInstance().logCritical("Session Control: All client FSMs gracefully closed.");
                }
            };

            global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(100), cb));
        }
    }
    void notify_removal_of_client(const client_id &cl_id) override
    {
        logger::getInstance().logInfo("Session Control: Channel Manager notified client removal for " + std::to_string(cl_id) + " (App side teardown).");
        trigger_teardown_step(cl_id);
    }

    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt, std::unique_ptr<transport_addr> source_addr) override
    {

        if (pkt->get_length() < rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + rudp_protocol_packet::SESSION_CONTROL_HEADER_SIZE)
        {
            logger::getInstance().logCritical("Received packet but too small for session control. Dropping.");
            return;
        }
        parse_session_control_packet_header(*pkt, *source_addr);

        auto cid_opt = clients_addr_to_id.get(*source_addr);
        if (cid_opt)
        {
            client_id cl_id = cid_opt.value();
            if (verify_can_exchange_data(cl_id))
            {
                if (auto cm_sp = channel_manager_ptr.lock())
                {

                    logger::getInstance().logTest("Session Control: Forwarding data packet to Channel Manager for client " + std::to_string(cl_id));
                    cm_sp->on_transport_receive(cl_id, std::move(pkt));
                }
            }
            else
            {
                logger::getInstance().logWarning("Session Control: Dropping data packet for client " + std::to_string(cl_id) + " due to FSM state.");
            }
        }
        else
        {

            logger::getInstance().logWarning("Session Control: Dropping data packet from unknown address " + source_addr->to_string());
        }
    }

    void on_transport_send_data(const client_id &cl_id, std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (verify_can_exchange_data(cl_id))
        {
            add_session_control_header(cl_id, *pkt);
            auto addr_opt = clients_id_to_addr.get(cl_id);
            if (addr_opt)
            {
                logger::getInstance().logInfo("Session Control: Sending data packet for client " + std::to_string(cl_id) + ", length " + std::to_string(pkt->get_length()));
                udp_ptr->send_packet_to_network(addr_opt.value(), pkt->get_buffer(), pkt->get_length());
            }
            else
            {
                logger::getInstance().logError("Session Control: Failed to send packet, address missing for client " + std::to_string(cl_id));
            }
        }
        else
        {
            logger::getInstance().logWarning("Session Control: Dropping outgoing data packet for client " + std::to_string(cl_id) + " due to FSM state.");
        }
    }
};