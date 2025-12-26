#pragma once

#include <unordered_map>
#include <memory>
#include <assert.h>
#include <mutex>
#include <atomic>
#include <sstream>
#include <iostream>

#include "i_udp_for_session_control.hpp"
#include "i_channel_manager_for_session_control.hpp"
#include "i_session_control_for_udp.hpp"
#include "i_session_control_for_channel_manager.hpp"

#include "../common/transport_addr.hpp"
#include "../common/rudp_protocol_packet.hpp"
#include "../common/i_timer_service.hpp"
#include "../common/thread_safe_unordered_map.hpp"
#include "../common/types.hpp"
#include "../common/logger.hpp"

class i_client;

struct session_control_header
{
    uint8_t flags;
    uint32_t reserved;
};

enum class CONNECTION_STATE
{
    CLOSED,
    SYN_SENT,
    ESTABLISHED,
    CLOSE_WAIT,
    LAST_ACK,
    TIME_WAIT
};

inline const char *connection_state_to_string(CONNECTION_STATE state)
{
    switch (state)
    {
    case CONNECTION_STATE::CLOSED:
        return "CLOSED";
    case CONNECTION_STATE::SYN_SENT:
        return "SYN_SENT";
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

inline std::string control_flags_to_string(uint8_t flags)
{
    std::string s;
    if (flags & static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::SYN))
        s += "SYN ";
    if (flags & static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::ACK))
        s += "ACK ";
    if (flags & static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::RST))
        s += "RST ";
    if (flags & static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::FIN))
        s += "FIN ";
    if (s.empty())
        return "NONE";
    if (s.back() == ' ')
        s.pop_back();
    return s;
}

class connection_state_machine : public std::enable_shared_from_this<connection_state_machine>
{
public:
    struct fsm_result
    {
        // since the callback handles it, but we keep it for immediate flow control logic.
        bool stop_data_exchange = false;
    };

    struct to_send_response
    {
        uint8_t response_flags = 0;
        bool to_send = false;
    };

    // Callback type: Notifies observer of state transitions
    using StateChangeCallback = std::function<void(CONNECTION_STATE, CONNECTION_STATE)>;

    connection_state_machine(std::function<void(uint8_t)> send_cb,
                             std::shared_ptr<i_timer_service> timer_man,
                             StateChangeCallback state_cb)
        : current_state(CONNECTION_STATE::SYN_SENT),
          on_send_control_packet_to_transport(send_cb),
          global_timer_manager(timer_man),
          on_state_change(state_cb)
    {
        LOG_INFO("[connection_state_machine] FSM initialized in state: " << connection_state_to_string(current_state.load()));
    }

    ~connection_state_machine()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);
        if (current_state.load() != CONNECTION_STATE::CLOSED)
        {
            LOG_WARN("[connection_state_machine] FSM destructed while active. Force closing.");
            // We don't call set_state_nolock here to avoid invoking callbacks during destruction
            // just send the RST if needed.
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking_nolock();
        }
    }

    // Special 5
    connection_state_machine(const connection_state_machine &) = delete;
    connection_state_machine &operator=(const connection_state_machine &) = delete;
    connection_state_machine(connection_state_machine &&) = delete;
    connection_state_machine &operator=(connection_state_machine &&) = delete;

    // ------------------------------------------------------------
    // PUBLIC API
    // ------------------------------------------------------------

    bool can_exchange_data()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);
        return current_state.load() == CONNECTION_STATE::SYN_SENT ||
               current_state.load() == CONNECTION_STATE::ESTABLISHED;
    }

    CONNECTION_STATE get_current_state() const
    {
        return current_state.load();
    }

    to_send_response get_to_send_response()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        if (current_state.load() == CONNECTION_STATE::SYN_SENT)
            last_response = {get_syn_flag(), true};

        auto toret = last_response;

        if ((toret.response_flags & get_ack_flag()) && toret.to_send)
            set_last_ack_sent_time_nolock();

        last_response.to_send = false;
        return toret;
    }

    void send_response_to_network_without_piggybacking()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);
        send_response_to_network_without_piggybacking_nolock();
    }

    void on_server_disconnected()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);
        set_state_nolock(CONNECTION_STATE::CLOSED);
    }

    fsm_result close()
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        CONNECTION_STATE old_state = current_state.load();

        if (old_state == CONNECTION_STATE::ESTABLISHED)
        {
            set_state_nolock(CONNECTION_STATE::LAST_ACK);
            LOG_INFO("[FSM] App requested close. ESTABLISHED -> LAST_ACK. Sending FIN.");
            last_ack_cb_with_retry_nolock();
        }
        else if (old_state != CONNECTION_STATE::TIME_WAIT)
        {
            LOG_WARN("[FSM] App requested close from state: " << connection_state_to_string(old_state) << ". Forcing CLOSED.");
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking_nolock();

            set_state_nolock(CONNECTION_STATE::CLOSED);
        }
        return {false};
    }

    fsm_result handle_change(uint8_t rcvd_flags)
    {
        std::lock_guard<std::mutex> lg(g_connection_state_machine_mutex);

        if (current_state.load() == CONNECTION_STATE::CLOSED)
        {
            // If we are closed, we just RST back anything that isn't a RST
            if (!(rcvd_flags & get_rst_flag()))
            {
                last_response = {get_rst_flag(), true};
                send_response_to_network_without_piggybacking_nolock();
            }
            return {false};
        }

        if (last_response.to_send)
            send_response_to_network_without_piggybacking_nolock();

        if (rcvd_flags & get_rst_flag())
        {
            LOG_CRITICAL("[FSM] Received RST. Forcing CLOSED.");
            set_state_nolock(CONNECTION_STATE::CLOSED);
            return {true};
        }

        CONNECTION_STATE old_state = current_state.load();

        switch (old_state)
        {
        case CONNECTION_STATE::SYN_SENT:
            if (rcvd_flags == get_ack_flag())
            {
                set_state_nolock(CONNECTION_STATE::ESTABLISHED);
                last_response = {0, false};
                LOG_INFO("[FSM] SYN_SENT -> ESTABLISHED (ACK).");
            }
            break;

        case CONNECTION_STATE::ESTABLISHED:
            if (rcvd_flags == get_fin_flag())
            {
                set_state_nolock(CONNECTION_STATE::TIME_WAIT);
                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking_nolock();
                LOG_INFO("[FSM] ESTABLISHED -> TIME_WAIT (FIN).");
                time_wait_cb_with_retry_nolock();
                return {true};
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
                LOG_INFO("[FSM] LAST_ACK -> CLOSED (ACK).");
                set_state_nolock(CONNECTION_STATE::CLOSED);
                return {false};
            }
            break;

        default:
            break;
        }

        return {false};
    }

private:
    std::mutex g_connection_state_machine_mutex;
    static constexpr uint32_t ROUND_TRIP_TIME = 2000;

    std::atomic<CONNECTION_STATE> current_state;
    std::function<void(uint8_t)> on_send_control_packet_to_transport;
    std::shared_ptr<i_timer_service> global_timer_manager;
    StateChangeCallback on_state_change;

    to_send_response last_response;
    TimerInfoTimePoint last_ack_send_time;

    // ------------------------------------------------------------
    // INTERNAL HELPERS
    // ------------------------------------------------------------

    uint8_t get_ack_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::ACK); }
    uint8_t get_rst_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::RST); }
    uint8_t get_fin_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::FIN); }
    uint8_t get_syn_flag() const { return uint8_t(CONTROL_PACKET_HEADER_FLAGS::SYN); }

    // CENTRAL STATE CHANGE HUB
    void set_state_nolock(CONNECTION_STATE new_state)
    {
        CONNECTION_STATE old = current_state.load();
        if (old != new_state)
        {
            current_state.store(new_state);
            on_state_change(old, new_state);
        }
    }

    void set_last_ack_sent_time_nolock()
    {
        last_ack_send_time = std::chrono::steady_clock::now();
    }

    void send_response_to_network_without_piggybacking_nolock()
    {
        if (!last_response.to_send)
            return;

        if (last_response.response_flags & get_ack_flag())
            set_last_ack_sent_time_nolock();

        LOG_INFO("[FSM] Sending standalone control flags: " << control_flags_to_string(last_response.response_flags));
        on_send_control_packet_to_transport(last_response.response_flags);
        last_response.to_send = false;
    }

    void last_ack_cb_with_retry_nolock(int retries_left = 5)
    {
        if (current_state.load() != CONNECTION_STATE::LAST_ACK || retries_left <= 0)
            return;

        LOG_INFO("[FSM] LAST_ACK retry " << std::to_string(6 - retries_left) << "/5");

        last_response = {get_fin_flag(), true};
        send_response_to_network_without_piggybacking_nolock();

        if (retries_left > 1)
        {
            std::weak_ptr<connection_state_machine> self_weak_ptr = shared_from_this();
            global_timer_manager->add_timer(std::make_shared<timer_info>(
                duration_ms(ROUND_TRIP_TIME * 2),
                [self_weak_ptr, retries_left]()
                {
                    if (auto sp = self_weak_ptr.lock())
                    {
                        std::lock_guard<std::mutex> lg(sp->g_connection_state_machine_mutex);
                        sp->last_ack_cb_with_retry_nolock(retries_left - 1);
                    }
                }));
        }
        else
        {
            LOG_WARN("[FSM] LAST_ACK retries exhausted. Forcing CLOSED.");
            set_state_nolock(CONNECTION_STATE::CLOSED);
        }
    }

    void time_wait_cb_with_retry_nolock(int retries_left = 5)
    {
        duration_ms time_spent = std::chrono::duration_cast<duration_ms>(
            std::chrono::steady_clock::now() - last_ack_send_time);

        if (retries_left > 0 && time_spent < duration_ms(ROUND_TRIP_TIME * 2))
        {
            std::weak_ptr<connection_state_machine> self_weak_ptr = shared_from_this();
            global_timer_manager->add_timer(std::make_shared<timer_info>(
                duration_ms(ROUND_TRIP_TIME * 2),
                [self_weak_ptr, retries_left]()
                {
                    if (auto sp = self_weak_ptr.lock())
                    {
                        std::lock_guard<std::mutex> lg(sp->g_connection_state_machine_mutex);
                        sp->time_wait_cb_with_retry_nolock(retries_left - 1);
                    }
                }));
        }
        else
        {
            LOG_INFO("[FSM] TIME_WAIT -> CLOSED (2*RTT elapsed).");
            set_state_nolock(CONNECTION_STATE::CLOSED);
        }
    }
};

class session_control : public i_session_control_for_udp, public i_session_control_for_channel_manager, public std::enable_shared_from_this<session_control>
{
    std::shared_ptr<i_timer_service> global_timer_manager;
    std::shared_ptr<i_udp_for_session_control> udp_ptr;
    std::weak_ptr<i_channel_manager_for_session_control> channel_manager_ptr;
    std::shared_ptr<connection_state_machine> client_fsm;

    std::atomic<int> teardown_counter = 0;

    void trigger_teardown_step()
    {
        int count = teardown_counter.fetch_add(1) + 1;
        LOG_INFO("Teardown step triggered. Counter: " << count);
    }

    void add_session_control_header(rudp_protocol_packet &pkt)
    {
        assert(client_fsm != nullptr);
        connection_state_machine::to_send_response res = client_fsm->get_to_send_response();
        session_control_header header;
        header.flags = 0, header.reserved = 0;

        if (res.to_send)
            header.flags |= res.response_flags;

        header.reserved = htonl(header.reserved);

        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET, &header.flags, sizeof(header.flags));
        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(header.flags), &header.reserved, sizeof(header.reserved));

        if (res.to_send)
        {
            LOG_INFO("Added session control header with flags: " << control_flags_to_string(res.response_flags));
        }
    }

    bool verify_can_exchange_data()
    {
        if (client_fsm && client_fsm->can_exchange_data())
            return true;
        LOG_WARN("Data exchange blocked. FSM state: " << connection_state_to_string(client_fsm ? client_fsm->get_current_state() : CONNECTION_STATE::CLOSED));
        return false;
    }

    void parse_session_control_packet_header(const rudp_protocol_packet &incoming_pkt)
    {

        uint8_t flags;
        uint32_t reserved_net;
        const uint8_t *buf = reinterpret_cast<const uint8_t *>(incoming_pkt.get_const_buffer());

        std::memcpy(&flags, buf, sizeof(flags));
        std::memcpy(&reserved_net, buf + sizeof(flags), sizeof(reserved_net));

        connection_state_machine::fsm_result res = client_fsm->handle_change(flags & ((uint8_t(1) << 4) - 1));

        LOG_INFO("Parsed flags: " << control_flags_to_string(flags & ((uint8_t(1) << 4) - 1))
                                  << ", Stop=" << res.stop_data_exchange);

        if (res.stop_data_exchange)
        {
            if (auto sp = channel_manager_ptr.lock())
            {
                sp->on_server_disconnected();
                trigger_teardown_step();
                LOG_CRITICAL("FSM indicated stop data exchange.");
            }
        }
    }

public:
    class client_setup_access_key
    {
        friend std::shared_ptr<i_client> create_client(const char *, const char *);

    private:
        client_setup_access_key() {}
    };

    session_control() { LOG_INFO("session_control object created."); }
    ~session_control() = default;

    // Special 5: Delete Copy and Move
    session_control(const session_control &) = delete;
    session_control &operator=(const session_control &) = delete;
    session_control(session_control &&) = delete;
    session_control &operator=(session_control &&) = delete;

    void initialize_fsm_after_dependencies()
    {
        client_fsm = std::make_shared<connection_state_machine>([this](uint8_t fsm_flags)
                                                                {
            char buf[rudp_protocol_packet::SESSION_CONTROL_HEADER_SIZE];
            uint32_t reserved = 0; 
            uint32_t net_reserved = htonl(reserved);

            memcpy(buf, &fsm_flags, sizeof(fsm_flags));
            memcpy(buf + sizeof(fsm_flags), &net_reserved, sizeof(net_reserved));

            this->udp_ptr->send_packet_to_network(buf, sizeof(buf)); 
            LOG_INFO("FSM triggered standalone control packet send with flags: " << control_flags_to_string(fsm_flags)); }, global_timer_manager,
                                                                [this](CONNECTION_STATE prev_state, CONNECTION_STATE new_state)
                                                                {
                if(new_state == CONNECTION_STATE::CLOSED && prev_state != CONNECTION_STATE::CLOSED)    {
                    this->trigger_teardown_step();
                } });
    }

    void set_i_timer_service(std::shared_ptr<i_timer_service> timer_man, client_setup_access_key) { global_timer_manager = timer_man; }
    void set_udp(std::shared_ptr<i_udp_for_session_control> udp_ptr_, client_setup_access_key)
    {
        udp_ptr = udp_ptr_;
        initialize_fsm_after_dependencies();
    }
    void set_channel_manager(std::weak_ptr<i_channel_manager_for_session_control> channel_manager_, client_setup_access_key)
    {
        channel_manager_ptr = channel_manager_;
    }

    void on_close_client() override
    {
        trigger_teardown_step();

        connection_state_machine::fsm_result res = client_fsm->close();

        LOG_INFO("Client closing. Waiting for background handshake...");

        int interval_ms = 10;

        while (teardown_counter.load() < 2)
        {

            LOG_CRITICAL(" Waiting for background handshake...");
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }

    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (!client_fsm)
        {
            LOG_CRITICAL("Received packet but client_fsm is null.");
            return;
        }
        if (pkt->get_length() < rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + rudp_protocol_packet::SESSION_CONTROL_HEADER_SIZE)
        {
            LOG_CRITICAL("Received packet too small.");
            return;
        }

        parse_session_control_packet_header(*pkt);

        if (verify_can_exchange_data())
        {
            if (auto cm_sp = channel_manager_ptr.lock())
            {
                cm_sp->on_transport_receive(std::move(pkt));
                LOG_INFO("Forwarding received data to channel manager.");
            }
            else
            {
                LOG_WARN("Channel manager is null.");
            }
        }
        else
        {
            LOG_WARN("Data exchange not allowed by FSM.");
        }
    }

    void on_server_disconnected() override
    {
        auto sp = channel_manager_ptr.lock();
        if (sp)
            sp->on_server_disconnected();
        client_fsm->on_server_disconnected();
    }

    void on_transport_send_data(std::shared_ptr<rudp_protocol_packet> pkt) override
    {
        if (!client_fsm)
        {
            LOG_CRITICAL("Attempted to send data but client_fsm is null.");
            return;
        }
        if (verify_can_exchange_data())
        {
            add_session_control_header(*pkt);
            udp_ptr->send_packet_to_network(pkt->get_buffer(), pkt->get_length());
            LOG_INFO("Sending data packet to network.");
        }
        else
        {
            LOG_WARN("Data exchange not allowed by FSM. Dropping.");
        }
    }
};