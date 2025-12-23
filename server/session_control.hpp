#pragma once

#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>

#include "../common/transport_addr.hpp"
#include "../common/i_timer_service.hpp"
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
struct connection_state_machine
    : public std::enable_shared_from_this<connection_state_machine>
{
    std::mutex g_connection_state_machine_mutex;

    static constexpr uint32_t ROUND_TRIP_TIME = 2000;

    std::atomic<CONNECTION_STATE> current_state = CONNECTION_STATE::LISTEN;

    std::function<void(uint8_t)> on_send_control_packet_to_transport;
    std::shared_ptr<i_timer_service> global_timer_manager;

    connection_state_machine(std::function<void(uint8_t)> f,
                             std::shared_ptr<i_timer_service> timer_man)
        : current_state(CONNECTION_STATE::LISTEN),
          on_send_control_packet_to_transport(f),
          global_timer_manager(timer_man)
    {
        LOG_INFO("[connection_state_machine] FSM created, state: LISTEN");
    }

    ~connection_state_machine()
    {
        LOG_INFO("[connection_state_machine] FSM destroyed.");
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

    // ------------------------------------------------
    // NOLOCK HELPERS
    // ------------------------------------------------

    void set_last_ack_sent_time_nolock()
    {
        last_ack_send_time = std::chrono::steady_clock::now();
    }

    void send_response_to_network_without_piggybacking_nolock()
    {
        if (!last_response.to_send)
            return;

        LOG_TEST("[FSM] Sending control packet: "
                 << flags_to_string(last_response.response_flags)
                 << " (Standalone)");

        if (last_response.response_flags & get_ack_flag())
            set_last_ack_sent_time_nolock();

        on_send_control_packet_to_transport(last_response.response_flags);
        last_response.to_send = false;
    }

    // ------------------------------------------------
    // PUBLIC API
    // ------------------------------------------------

    bool can_exchange_data()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);
        return current_state.load() == CONNECTION_STATE::ESTABLISHED;
    }

    to_send_response get_to_send_response()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        auto toret = last_response;

        if ((toret.response_flags & get_ack_flag()) && toret.to_send)
        {
            set_last_ack_sent_time_nolock();
            LOG_TEST("[FSM] Piggybacking "
                     << flags_to_string(toret.response_flags));
        }

        last_response.to_send = false;
        return toret;
    }

    void send_response_to_network_without_piggybacking()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);
        send_response_to_network_without_piggybacking_nolock();
    }

    void last_ack_cb_with_retry(int retries_left = 10)
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        if (current_state.load() != CONNECTION_STATE::LAST_ACK || retries_left <= 0)
            return;

        LOG_INFO("[FSM] LAST_ACK retry "
                 << std::to_string(6 - retries_left) << "/5");

        last_response = {get_fin_flag(), true};
        send_response_to_network_without_piggybacking_nolock();

        if (retries_left > 0)
        {
            std::weak_ptr<connection_state_machine> self = shared_from_this();

            global_timer_manager->add_timer(std::make_shared<timer_info>(
                duration_ms(ROUND_TRIP_TIME * 2),
                [self, retries_left]()
                {
                    if (auto sp = self.lock())
                        sp->last_ack_cb_with_retry(retries_left - 1);
                }));
        }
        else
        {
            LOG_WARN("[FSM] LAST_ACK retries exhausted");
        }
    }

    void time_wait_cb_with_retry(int retries_left = 5)
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        auto time_spent = std::chrono::duration_cast<duration_ms>(
            std::chrono::steady_clock::now() - last_ack_send_time);

        if (retries_left > 0 &&
            time_spent < duration_ms(ROUND_TRIP_TIME * 2))
        {
            std::weak_ptr<connection_state_machine> self = shared_from_this();

            global_timer_manager->add_timer(std::make_unique<timer_info>(
                duration_ms(ROUND_TRIP_TIME * 2),
                [self, retries_left]()
                {
                    if (auto sp = self.lock())
                        sp->time_wait_cb_with_retry(retries_left - 1);
                }));
        }
        else
        {
            current_state.store(CONNECTION_STATE::CLOSED);
            LOG_INFO("[FSM] TIME_WAIT -> CLOSED (2*RTT elapsed)");
        }
    }

    uint8_t get_ack_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::ACK); }
    uint8_t get_rst_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::RST); }
    uint8_t get_fin_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::FIN); }
    uint8_t get_syn_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::SYN); }

    fsm_result close()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        LOG_INFO("[FSM::close] State: "
                 << state_to_string(current_state.load()));

        if (current_state.load() == CONNECTION_STATE::ESTABLISHED)
        {
            current_state.store(CONNECTION_STATE::LAST_ACK);
            LOG_INFO("[FSM] ESTABLISHED -> LAST_ACK");

            last_ack_cb_with_retry();
            return {false, false};
        }
        else
        {
            LOG_ERROR("[FSM] close() from invalid state, sending RST");

            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking_nolock();

            current_state.store(CONNECTION_STATE::CLOSED);
            return {true, false};
        }
    }

    fsm_result handle_change(uint8_t rcvd_flags)
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        LOG_TEST("[FSM] handle_change: "
                 << state_to_string(current_state.load())
                 << " recv=" << flags_to_string(rcvd_flags));

        if (current_state.load() == CONNECTION_STATE::CLOSED)
        {
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking_nolock();
            return {false, false};
        }

        if (last_response.to_send)
            send_response_to_network_without_piggybacking_nolock();

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
                send_response_to_network_without_piggybacking_nolock();
                time_wait_cb_with_retry();
                return {false, true};
            }
            else if (rcvd_flags == get_syn_flag())
            {
                last_response = {get_ack_flag(), true};
            }
            break;

        case CONNECTION_STATE::TIME_WAIT:
            if (rcvd_flags == get_fin_flag())
            {
                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking_nolock();
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

    std::shared_ptr<i_timer_service> global_timer_manager;
    std::shared_ptr<i_udp_for_session_control> udp_ptr;
    std::weak_ptr<i_channel_manager_for_session_control> channel_manager_ptr;
    thread_safe_unordered_map<transport_addr, client_id, transport_addr_hasher> clients_addr_to_id;
    thread_safe_unordered_map<client_id, transport_addr> clients_id_to_addr;
    thread_safe_unordered_map<client_id, std::shared_ptr<connection_state_machine>> clients_fsm;

    thread_safe_unordered_map<client_id, std::shared_ptr<std::atomic<int>>> teardown_counter;

    void perform_final_cleanup(client_id cl_id)
    {
        LOG_INFO("Session Control: Performing final cleanup for client " << cl_id);
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
            LOG_ERROR("Session Control: Teardown triggered for client " << cl_id << " but counter missing.");
            return;
        }

        int previous_value = (*counter)->fetch_add(1);
        LOG_TEST("Session Control: Teardown step triggered for client " << cl_id << ". Counter: " << (previous_value + 1));

        if (previous_value == 1)
        {
            LOG_INFO("Session Control: Both Teardown flags set (Net+App). Final cleanup for client " << cl_id);
            perform_final_cleanup(cl_id);
        }
    }

    void add_session_control_header(const client_id &cl_id, rudp_protocol_packet &pkt)
    {
        auto fsm_opt = clients_fsm.get(cl_id);
        if (!fsm_opt)
        {
            LOG_ERROR("Session Control: Failed to add header, FSM missing for client " << cl_id);
            return;
        }
        connection_state_machine::to_send_response res = fsm_opt.value()->get_to_send_response();
        session_control_header header;
        header.flags = 0;
        header.reserved = 0;

        if (res.to_send)
        {
            header.flags |= res.response_flags;
            LOG_TEST("Session Control: Piggybacking flags " << flags_to_string(res.response_flags) << " on client " << cl_id << " packet.");
        }

        uint32_t net_reserved = htonl(header.reserved);

        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET, &header.flags, sizeof(header.flags));
        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(header.flags), &net_reserved, sizeof(net_reserved));
    }

    bool verify_can_exchange_data(const client_id &cl_id)
    {
        if (server_closed.load())
        {
            LOG_WARN("Session Control: Data exchange blocked (Server Closed).");
            return false;
        }
        auto fsm_opt = clients_fsm.get(cl_id);
        if (!fsm_opt)
        {
            LOG_ERROR("Session Control: Data exchange blocked, FSM missing for client " << cl_id);
            return false;
        }
        return fsm_opt.value()->can_exchange_data();
    }

    void parse_session_control_packet_header(const rudp_protocol_packet &incoming_pkt, const transport_addr &source_addr)
    {
        uint8_t flags = *reinterpret_cast<const uint8_t *>(incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET);

        const char *buf =
            incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET;

        uint32_t reserved_net;
        std::memcpy(&reserved_net, buf + sizeof(flags), sizeof(reserved_net));
        uint32_t reserved = ntohl(reserved_net);

        uint8_t control_flags = flags & ((uint8_t(1) << 4) - 1);
        LOG_TEST("Session Control: Received packet from " << source_addr.to_string() << ". Flags: " << flags_to_string(control_flags));

        if (!clients_addr_to_id.contains(source_addr))
        {
            if (server_closed.load())
            {
                LOG_WARN("Session Control: Ignoring connection attempt (Server Closed).");
                return;
            }

            client_id cl_id = INVALID_CLIENT_ID;

            do
            {
                cl_id = get_random_client_id();
            } while (clients_id_to_addr.contains(cl_id) || (cl_id == INVALID_CLIENT_ID));

            LOG_INFO("Session Control: New client detected at " << source_addr.to_string() << ". Assigned ID: " << cl_id);

            teardown_counter.insert(cl_id, std::make_shared<std::atomic<int>>(0));
            create_fsm_for_client(cl_id, source_addr);

            auto fsm_opt = clients_fsm.get(cl_id);
            if (!fsm_opt)
            {
                LOG_ERROR("Session Control: FSM creation failed for new client " << cl_id);
                return;
            }

            connection_state_machine::fsm_result res = fsm_opt.value()->handle_change(control_flags);

            if (res.close_connection)
            {
                LOG_WARN("Session Control: FSM immediately closed connection for new client. Erasing FSM/ID.");
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
                        LOG_INFO("Session Control: New client " << cl_id << " notified to Channel Manager.");
                    }
                }
            }
        }
        else
        {
            auto cid_opt = clients_addr_to_id.get(source_addr);
            if (!cid_opt)
            {
                LOG_ERROR("Session Control: Lookup failed for existing address " << source_addr.to_string());
                return;
            }
            client_id cl_id = cid_opt.value();

            auto fsm_opt = clients_fsm.get(cl_id);
            if (!fsm_opt)
            {
                LOG_WARN("Session Control: Received packet from known address, but FSM missing for client " << cl_id << ". Ignoring.");
                return;
            }

            connection_state_machine::fsm_result res = fsm_opt.value()->handle_change(control_flags);

            if (res.close_connection)
            {
                if (!teardown_counter.contains(cl_id))
                {
                    LOG_ERROR("Session Control: Missing counter during close_connection for client " << cl_id << ". Created new counter.");
                }

                trigger_teardown_step(cl_id);
            }

            if (res.stop_data_exchange)
            {
                if (auto sp = channel_manager_ptr.lock())
                {
                    if (!teardown_counter.contains(cl_id))
                    {
                        LOG_ERROR("Session Control: Missing counter during stop_data_exchange for client " << cl_id << ". Created new counter.");
                    }

                    sp->remove_client(cl_id);
                    LOG_INFO("Session Control: Notifying Channel Manager of client removal for " << cl_id);
                }
            }
        }
    }

    void create_fsm_for_client(const client_id &cl_id, const transport_addr &source_addr)
    {
        clients_fsm.insert(cl_id, std::make_shared<connection_state_machine>([source_addr, cl_id, this](uint8_t fsm_flags)
                                                                             {
                                                                                char buf[rudp_protocol_packet::SESSION_CONTROL_HEADER_SIZE] = {0};

                                                                                uint32_t net_reserved = htonl(0);
                                                                                
                                                                                
                                                                                memcpy(buf, &fsm_flags, sizeof(fsm_flags));
                                                                                
                                                                                memcpy(buf + sizeof(fsm_flags), &net_reserved, sizeof(net_reserved)); 
                                                                                
                                                                                LOG_INFO("Control Packet Send: Client " << cl_id << ", Flags: " << flags_to_string(fsm_flags));
                                                                                this->udp_ptr->send_packet_to_network(source_addr, buf, sizeof(buf)); }, global_timer_manager));
    }

public:
    class server_setup_access_key
    {
        friend std::shared_ptr<i_server> create_server(const char *);

    private:
        server_setup_access_key() {}
    };

    void set_i_timer_service(std::shared_ptr<i_timer_service> timer_man, server_setup_access_key)
    {
        global_timer_manager = timer_man;
        LOG_INFO("Session Control: Timer Manager set.");
    }
    void set_udp(std::shared_ptr<i_udp_for_session_control> udp_ptr_, server_setup_access_key)
    {
        udp_ptr = udp_ptr_;
        LOG_INFO("Session Control: UDP Transport set.");
    }
    void set_channel_manager(std::weak_ptr<i_channel_manager_for_session_control> channel_manager_, server_setup_access_key)
    {
        channel_manager_ptr = channel_manager_;
        LOG_INFO("Session Control: Channel Manager set.");
    }

    void on_close_server() override
    {
        LOG_CRITICAL("Session Control: Server closure initiated.");
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
                LOG_INFO("Session Control: Client " << cl_id << " FSM closed immediately. Preparing for cleanup.");
            }
            else
            {
                trigger_teardown_step(cl_id);
                LOG_INFO("Session Control: Client " << cl_id << " starting graceful teardown (Counter=1).");
            }
        }
        for (auto i : to_clean_up)
            perform_final_cleanup(i);

        if (clients_fsm.size() > 0)
        {
            LOG_INFO("Session Control: Waiting for graceful FSM closures. Starting poll timer.");
            auto this_shared_ptr = shared_from_this();
            std::function<void()> cb = [this_shared_ptr, cb]
            {
                if (this_shared_ptr->clients_fsm.size() > 0)
                {
                    this_shared_ptr->global_timer_manager->add_timer(std::make_shared<timer_info>(duration_ms(100), cb));
                }
                else
                {
                    LOG_CRITICAL("Session Control: All client FSMs gracefully closed.");
                }
            };

            global_timer_manager->add_timer(std::make_shared<timer_info>(duration_ms(100), cb));
        }
    }
    void notify_removal_of_client(const client_id &cl_id) override
    {
        LOG_INFO("Session Control: Channel Manager notified client removal for " << cl_id << " (App side teardown).");
        trigger_teardown_step(cl_id);
    }

    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt, std::unique_ptr<transport_addr> source_addr) override
    {

        if (pkt->get_length() < rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + rudp_protocol_packet::SESSION_CONTROL_HEADER_SIZE)
        {
            LOG_CRITICAL("Received packet but too small for session control. Dropping.");
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

                    LOG_TEST("Session Control: Forwarding data packet to Channel Manager for client " << cl_id);
                    cm_sp->on_transport_receive(cl_id, std::move(pkt));
                }
            }
            else
            {
                LOG_WARN("Session Control: Dropping data packet for client " << cl_id << " due to FSM state.");
            }
        }
        else
        {

            LOG_WARN("Session Control: Dropping data packet from unknown address " << source_addr->to_string());
        }
    }

    void on_transport_send_data(const client_id &cl_id, std::shared_ptr<rudp_protocol_packet> pkt) override
    {
        if (verify_can_exchange_data(cl_id))
        {
            add_session_control_header(cl_id, *pkt);
            auto addr_opt = clients_id_to_addr.get(cl_id);
            if (addr_opt)
            {
                LOG_INFO("Session Control: Sending data packet for client " << cl_id << ", length " << pkt->get_length());
                udp_ptr->send_packet_to_network(addr_opt.value(), pkt->get_buffer(), pkt->get_length());
            }
            else
            {
                LOG_ERROR("Session Control: Failed to send packet, address missing for client " << cl_id);
            }
        }
        else
        {
            LOG_WARN("Session Control: Dropping outgoing data packet for client " << cl_id << " due to FSM state.");
        }
    }
};