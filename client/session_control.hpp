#pragma once

#include <unordered_map>
#include <memory>
#include <assert.h>
#include <mutex>
#include <atomic>
#include <sstream>
#include <iostream>

#include "../common/rudp_protocol_packet.hpp"
#include "i_udp_for_session_control.hpp"
#include "i_channel_manager_for_session_control.hpp"
#include "i_session_control_for_udp.hpp"
#include "i_session_control_for_channel_manager.hpp"
#include "transport_addr.hpp"
#include "rudp_protocol.hpp"
#include "../common/timer_manager.hpp"
#include "../common/thread_safe_unordered_map.hpp"
#include "types.hpp"
#include "../common/logger.hpp"

class i_client;

enum class CONNECTION_STATE
{
    CLOSED,
    SYN_SENT,
    ESTABLISHED,
    CLOSE_WAIT,
    LAST_ACK,
    TIME_WAIT
};

const char *connection_state_to_string(CONNECTION_STATE state)
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

std::string control_flags_to_string(uint8_t flags)
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

struct connection_state_machine : public std::enable_shared_from_this<connection_state_machine>
{
    std::recursive_mutex g_connection_state_machine_mutex;

    static constexpr uint32_t ROUND_TRIP_TIME = 2000; // ℹ️ NEED TO KNOWx`

    std::atomic<CONNECTION_STATE> current_state = CONNECTION_STATE::SYN_SENT;

    std::function<void(uint8_t)> on_send_control_packet_to_transport;
    std::shared_ptr<timer_manager> global_timer_manager;

    connection_state_machine(std::function<void(uint8_t)> f, std::shared_ptr<timer_manager> timer_man)
        : current_state(CONNECTION_STATE::SYN_SENT), on_send_control_packet_to_transport(f), global_timer_manager(timer_man)
    {
        std::ostringstream oss;
        oss << "FSM initialized in state: " << connection_state_to_string(current_state.load());
        logger::getInstance().logInfo(oss.str());
    }

    ~connection_state_machine()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        if (current_state.load() != CONNECTION_STATE::CLOSED)
        {
            std::ostringstream oss;
            oss << "FSM destructed while in state: " << connection_state_to_string(current_state.load()) << ". Sending RST.";
            logger::getInstance().logWarning(oss.str());
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();
        }
        else
        {
            logger::getInstance().logInfo("FSM destructed gracefully in CLOSED state.");
        }
    }
    bool can_exchange_data()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        return current_state.load() == CONNECTION_STATE::SYN_SENT || current_state.load() == CONNECTION_STATE::ESTABLISHED;
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

    void set_last_ack_sent_time()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        last_ack_send_time = std::chrono::steady_clock::now();
    }

    to_send_response get_to_send_response()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);

        if (current_state.load() == CONNECTION_STATE::SYN_SENT)
            last_response = {get_syn_flag(), true};

        auto toret = last_response;

        if ((toret.response_flags & (get_ack_flag())) && toret.to_send)
            set_last_ack_sent_time();

        last_response.to_send = false;

        if (toret.to_send)
        {
            std::ostringstream oss;
            oss << "Piggybacking control flags: " << control_flags_to_string(toret.response_flags) << ". Current state: " << connection_state_to_string(current_state.load());
            logger::getInstance().logInfo(oss.str());
        }

        return toret;
    }

    void send_response_to_network_without_piggybacking()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        if (last_response.to_send)
        {
            if (last_response.response_flags & (get_ack_flag()))
                set_last_ack_sent_time();

            std::ostringstream oss;
            oss << "Sending standalone control packet with flags: " << control_flags_to_string(last_response.response_flags) << ". Current state: " << connection_state_to_string(current_state.load());
            logger::getInstance().logInfo(oss.str());

            on_send_control_packet_to_transport(last_response.response_flags);
            last_response.to_send = false;
        }
    }

    uint8_t get_ack_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::ACK); }
    uint8_t get_rst_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::RST); }
    uint8_t get_fin_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::FIN); }
    uint8_t get_syn_flag() const { return static_cast<uint8_t>(CONTROL_PACKET_HEADER_FLAGS::SYN); }

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

    void on_server_disconnected()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        current_state.store(CONNECTION_STATE::CLOSED);
    }

    fsm_result close()
    {
        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);
        CONNECTION_STATE old_state = current_state.load();

        if (old_state == CONNECTION_STATE::ESTABLISHED)
        {
            current_state.store(CONNECTION_STATE::LAST_ACK);
            logger::getInstance().logInfo("Application requested close. Transitioning to LAST_ACK state. Sending FIN.");

            last_ack_cb_with_retry();

            return {false, false};
        }

        else
        {
            std::ostringstream oss;
            oss << "Application requested close from state: " << connection_state_to_string(old_state) << ". Sending RST and forcing CLOSED.";
            logger::getInstance().logWarning(oss.str());
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();

            current_state.store(CONNECTION_STATE::CLOSED);
            return {true, false};
        }
    }

    fsm_result handle_change(uint8_t rcvd_flags)
    {

        std::lock_guard<std::recursive_mutex> lg(g_connection_state_machine_mutex);

        std::ostringstream oss_rcv;
        oss_rcv << "Received flags: " << control_flags_to_string(rcvd_flags) << ". Current state: " << connection_state_to_string(current_state.load());
        logger::getInstance().logInfo(oss_rcv.str());

        if (current_state.load() == CONNECTION_STATE::CLOSED)
        {
            logger::getInstance().logWarning("Received packet in CLOSED state. Sending RST and ignoring.");
            last_response = {get_rst_flag(), true};
            send_response_to_network_without_piggybacking();
            return {false, false};
        }

        if (last_response.to_send)
        {
            logger::getInstance().logWarning("Pending control packet to send. Sending it now without piggybacking to avoid delay.");
            send_response_to_network_without_piggybacking();
            // After sending, continue processing the received packet to avoid unnecessary looping/delay on the receiver side
        }

        if (rcvd_flags & get_rst_flag())
        {
            logger::getInstance().logCritical("Received RST flag. Forcing transition to CLOSED.");
            current_state.store(CONNECTION_STATE::CLOSED);
            return {true, true};
        }

        CONNECTION_STATE old_state = current_state.load();

        switch (old_state)
        {
        case CONNECTION_STATE::SYN_SENT:
            if (rcvd_flags == get_ack_flag())
            {
                current_state.store(CONNECTION_STATE::ESTABLISHED);
                last_response = {0, false};
                logger::getInstance().logInfo("Transition: SYN_SENT -> ESTABLISHED (Received ACK).");
            }
            else
            {
                std::ostringstream oss;
                oss << "SYN_SENT: Unexpected flags received: " << control_flags_to_string(rcvd_flags) << ". Ignoring.";
                logger::getInstance().logWarning(oss.str());
            }
            break;

        case CONNECTION_STATE::ESTABLISHED:
            if (rcvd_flags == get_fin_flag())
            {
                current_state.store(CONNECTION_STATE::TIME_WAIT);

                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking();
                logger::getInstance().logInfo("Transition: ESTABLISHED -> TIME_WAIT (Received FIN, sending ACK).");

                time_wait_cb_with_retry();

                return {false, true};
            }
            break;

        case CONNECTION_STATE::TIME_WAIT:
            if (rcvd_flags == get_fin_flag())
            {
                // means ack got lost for fin
                last_response = {get_ack_flag(), true};
                send_response_to_network_without_piggybacking();
                logger::getInstance().logWarning("TIME_WAIT: Received duplicate FIN (ACK likely lost). Re-sending ACK.");
            }
            break;

        case CONNECTION_STATE::LAST_ACK:
            if (rcvd_flags == get_ack_flag())
            {
                current_state.store(CONNECTION_STATE::CLOSED);
                logger::getInstance().logInfo("Transition: LAST_ACK -> CLOSED (Received ACK for FIN).");
                return {true, false};
            }
            break;

        default:
            std::ostringstream oss;
            oss << "FSM in unhandled state: " << connection_state_to_string(current_state.load()) << " with flags: " << control_flags_to_string(rcvd_flags) << ". Ignoring.";
            logger::getInstance().logError(oss.str());
            break;
        }

        return {false, false};
    }
};

class session_control : public i_session_control_for_udp, public i_session_control_for_channel_manager, public std::enable_shared_from_this<session_control>
{

    std::shared_ptr<timer_manager> global_timer_manager;
    std::shared_ptr<i_udp_for_session_control> udp_ptr;
    std::weak_ptr<i_channel_manager_for_session_control> channel_manager_ptr;
    std::shared_ptr<connection_state_machine> client_fsm;

    std::shared_ptr<std::atomic<int>> teardown_counter = std::make_shared<std::atomic<int>>(0);

    void trigger_teardown_step()
    {
        if (!teardown_counter)
            return;
        int count = teardown_counter->fetch_add(1) + 1;
        std::ostringstream oss;
        oss << "Teardown step triggered. Counter: " << count;
        logger::getInstance().logInfo(oss.str());
    }

    void add_session_control_header(rudp_protocol_packet &pkt)
    {
        assert(client_fsm != nullptr);

        connection_state_machine::to_send_response res = client_fsm->get_to_send_response();
        rudp_protocol::session_control_header header;
        header.flags = 0, header.reserved = 0;

        if (res.to_send)
            header.flags |= res.response_flags;

        header.reserved = htonl(header.reserved);
        //
        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET, &header.flags, sizeof(header.flags));
        memcpy(pkt.get_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(header.flags), &header.reserved, sizeof(header.reserved));

        if (res.to_send)
        {
            std::ostringstream oss;
            oss << "Added session control header with flags: " << control_flags_to_string(res.response_flags) << " to outgoing packet.";
            logger::getInstance().logInfo(oss.str());
        }
    }

    bool verify_can_exchange_data()
    {
        bool can_exchange = client_fsm && client_fsm->can_exchange_data();
        if (!can_exchange)
        {
            std::ostringstream oss;
            oss << "Data exchange blocked. Current FSM state: " << connection_state_to_string(client_fsm ? client_fsm->current_state.load() : CONNECTION_STATE::CLOSED);
            logger::getInstance().logWarning(oss.str());
        }
        return can_exchange;
    }

    void parse_session_control_packet_header(const rudp_protocol_packet &incoming_pkt)
    {
        if (!client_fsm)
        {
            logger::getInstance().logCritical("FSM is null during packet parse. Dropping packet.");
            return;
        }

        uint8_t flags = *reinterpret_cast<const uint8_t *>(incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET);
        uint32_t reserved = *reinterpret_cast<const uint32_t *>(incoming_pkt.get_const_buffer() + rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + sizeof(flags));
        reserved = ntohl(reserved);

        connection_state_machine::fsm_result res = client_fsm->handle_change(flags & ((uint8_t(1) << 4) - 1));
        std::ostringstream oss;
        oss << "Parsed session control header with flags: " << control_flags_to_string(flags & ((uint8_t(1) << 4) - 1)) << ". FSM result: close_connection=" << res.close_connection << ", stop_data_exchange=" << res.stop_data_exchange;
        logger::getInstance().logInfo(oss.str());

        if (res.close_connection)
            trigger_teardown_step();

        if (res.stop_data_exchange)
        {
            if (auto sp = channel_manager_ptr.lock())
            {
                sp->on_server_disconnected();
                trigger_teardown_step();
                logger::getInstance().logCritical("FSM indicated stop data exchange. Notifying channel manager of server disconnect.");
            }
        }
    }

public:
    // selective access
    class client_setup_access_key
    {
        friend std::shared_ptr<i_client> create_client(const char *, const char *);

    private:
        client_setup_access_key() {}
    };

    session_control()
    {
        logger::getInstance().logInfo("session_control object created.");
    }

    // Defer FSM creation until timer_manager is set
    void initialize_fsm_after_dependencies()
    {
        client_fsm = std::make_shared<connection_state_machine>([this](uint8_t fsm_flags)
                                                                {
            char buf[rudp_protocol::SESSION_CONTROL_HEADER_SIZE];
            uint32_t reserved = 0; // Not used currently, but part of the header struct
            uint32_t net_reserved = htonl(reserved);

            memcpy(buf, &fsm_flags, sizeof(fsm_flags));
            memcpy(buf + sizeof(fsm_flags), &net_reserved, sizeof(net_reserved));

            this->udp_ptr->send_packet_to_network(buf, sizeof(buf)); 
            std::ostringstream oss;
            oss << "FSM triggered standalone control packet send with flags: " << control_flags_to_string(fsm_flags);
            logger::getInstance().logInfo(oss.str()); }, global_timer_manager);
    }

    void set_timer_manager(std::shared_ptr<timer_manager> timer_man, client_setup_access_key) { global_timer_manager = timer_man; }
    void set_udp(std::shared_ptr<i_udp_for_session_control> udp_ptr_, client_setup_access_key)
    {
        udp_ptr = udp_ptr_;
        initialize_fsm_after_dependencies();
    }
    void set_channel_manager(std::weak_ptr<i_channel_manager_for_session_control> channel_manager_, client_setup_access_key)
    {
        channel_manager_ptr = channel_manager_;
        // Now that all dependencies are set, initialize FSM (assuming client creation handles this)
    }
    void on_close_client() override
    {
        trigger_teardown_step();

        connection_state_machine::fsm_result res = client_fsm->close();
        if (res.close_connection)
            trigger_teardown_step();

        logger::getInstance().logInfo("Teardowncounter value : ." + std::to_string(teardown_counter->load()));

        if (teardown_counter->load() < 2)
        {
            logger::getInstance().logInfo("Teardown not complete. Starting polling timer.");
            auto this_shared_ptr = shared_from_this();
            std::function<void()> cb = [this_shared_ptr, cb]
            {
                if (this_shared_ptr->teardown_counter->load() < 2)
                {
                    logger::getInstance().logInfo("Teardown polling timer tick. Counter < 2. Rescheduling.");
                    this_shared_ptr->global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(100), cb));
                }
                else
                {
                    logger::getInstance().logInfo("Teardown complete (Counter >= 2). Stopping polling.");
                }
            };

            global_timer_manager->add_timer(std::make_unique<timer_info>(duration_ms(100), cb));
        } // this shared ptr will keep session control alive until all fsms gracefully close
    }

    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (!client_fsm)
        {
            logger::getInstance().logCritical("Received packet but client_fsm is null. Dropping.");
            return;
        }

        if (pkt->get_length() < rudp_protocol_packet::SESSION_CONTROL_HEADER_OFFSET + rudp_protocol_packet::SESSION_CONTROL_HEADER_SIZE)
        {
            logger::getInstance().logCritical("Received packet but too small for session control. Dropping.");
            return;
        }

        parse_session_control_packet_header(*pkt);

        if (verify_can_exchange_data())
            if (auto cm_sp = channel_manager_ptr.lock())
            {
                cm_sp->on_transport_receive(std::move(pkt));
                logger::getInstance().logInfo("Forwarding received data to channel manager.");
            }
            else
            {
                logger::getInstance().logWarning("Data received, FSM allows exchange, but channel manager is null/expired. Dropping data.");
            }
        else
        {
            logger::getInstance().logWarning("Data exchange not allowed by FSM. Dropping received data packet.");
        }
    }
    void on_server_disconnected() override
    {
        auto sp = channel_manager_ptr.lock();
        if (sp)
            sp->on_server_disconnected();
        client_fsm->on_server_disconnected();
    }

    void on_transport_send_data(std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (!client_fsm)
        {
            logger::getInstance().logCritical("Attempted to send data but client_fsm is null. Dropping.");
            return;
        }

        if (verify_can_exchange_data())
        {
            add_session_control_header(*pkt);
            udp_ptr->send_packet_to_network(pkt->get_buffer(), pkt->get_length());
            logger::getInstance().logInfo("Sending data packet to network via UDP.");
        }
        else
        {
            logger::getInstance().logWarning("Data exchange not allowed by FSM. Dropping data packet for transmission.");
        }
    }
};