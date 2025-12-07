#pragma once

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <memory>
#include <memory.h>
#include <unordered_map>
#include <set>
#include <random>
#include <thread>
#include <string.h>
#include <utility>

#include "types.hpp"
#include "thread_safe_unordered_set.hpp"
#include "timer_info.hpp"
#include "channel.hpp"
#include "udp.hpp"
#include "rudp_protocol.hpp"
#include "channels/realible_ordered_channel/reliable_ordered_channel.hpp"
#include "i_server.hpp"
#include "i_udp_callback.hpp"
#include "i_session_control_callback.hpp"
#include "i_sockaddr.hpp"
#include "thread_safe_priority_queue.hpp"

class channel_manager : public i_server, public i_udp_callback, public i_session_control_callback
{

    /*
    here channels add their own id when they are ready to give app stuff,
    they will have ptr to thread_safe_unordered_set
    channel manager will take out stuff
    */
    static constexpr uint32_t MAX_CHANNELS = 2048;

    std::shared_ptr<thread_safe_unordered_set<std::pair<client_id, channel_id>>> ready_list;
    std::shared_ptr<udp> udp_transport;

    using timer_info_ptr = std::shared_ptr<timer_info>;
    struct timer_info_ptr_compare
    {
        bool operator()(const timer_info_ptr &a, const timer_info_ptr &b) const
        {
            return *b < *a;
        }
    };
    thread_safe_priority_queue<timer_info_ptr, std::vector<timer_info_ptr>, timer_info_ptr_compare> timers;

    std::unordered_map<channel_id, std::unique_ptr<channel>> channels;
    std::unordered_map<client_id, std::unordered_map<channel_id, std::unique_ptr<channel>>> per_client_channels;
    std::unordered_map<client_id, std::shared_ptr<i_sockaddr>> client_sockaddr;

    std::jthread event_loop_thread;
    void event_loop(std::stop_token token)
    {
        timer_info_ptr cur_timer_ptr = nullptr;

        while (!token.stop_requested())
        {

            bool status = timers.wait_for_and_top(cur_timer_ptr, duration_ms(100));

            if (!status)
                continue;

            if (token.stop_requested())
                return;

            /*
             * !!! CONCURRENCY ANNOTATION / FLOW CONTROL RELIANCE !!!
             * * This logic relies on the following two assumptions for correctness:
             * * 1. ATOMICITY ASSUMPTION: The check (timers.top) and the removal (timers.pop)
             * are NOT atomic. A race condition is possible between the two calls.
             * 2. FUNCTIONAL ASSUMPTION: We rely on the property that IF the timer we check (A)
             * has expired, THEN the highest-priority timer (B) that may have been
             * pushed during the race is ALSO expired.
             * * RATIONALE: We skip the logic to verify the popped item matches the peeked item.
             * If the queue is raced, the highest priority item (B) is processed, and the
             * peeked item (A) is left in the queue for the next iteration.
             * * WARNING: DO NOT ADD ANY TIME-CONSUMING OR LOCK-RELEASING OPERATION HERE.
             * The correct, robust fix is to use timers.pop_if_expired() if you want specific other functionality ( implement it urself ).
             */

            // even if something new got added, its expiration must be smaller to come in front
            if (cur_timer_ptr->has_expired())
            {
                cur_timer_ptr = timers.wait_and_pop();
                cur_timer_ptr->execute_on_expire_callback();
            }
            else
                std::this_thread::sleep_for(std::min(duration_ms(100), cur_timer_ptr->time_remaining_in_ms()));
        }
    }

    void serialize_common_header(char *buf, const size_t &len, const rudp_protocol::common_header &c_header)
    {

        assert(len >= rudp_protocol::COMMON_HEADER_SIZE);

        uint64_t ncl_id = static_cast<uint64_t>(c_header.cl_id);
        uint16_t nch_id = static_cast<uint16_t>(c_header.ch_id);

        ncl_id = rudp_protocol::htonll(ncl_id);
        nch_id = htons(nch_id);
        //
        memcpy(buf + rudp_protocol::common_header::CLIENT_ID_OFSET, &ncl_id, sizeof(uint16_t));
        memcpy(buf + rudp_protocol::common_header::CHANNEL_ID_OFFSET, &nch_id, sizeof(uint16_t));
    }
    rudp_protocol::common_header deserialize_common_header(const char *buf, const size_t &len)
    {
        assert(len >= rudp_protocol::COMMON_HEADER_SIZE);

        uint64_t ncl_id = *reinterpret_cast<const uint64_t *>(buf + rudp_protocol::common_header::CLIENT_ID_OFSET);
        uint16_t nch_id = *reinterpret_cast<const uint16_t *>(buf + rudp_protocol::common_header::CHANNEL_ID_OFFSET);

        ncl_id = rudp_protocol::ntohll(ncl_id);
        nch_id = ntohs(nch_id);

        return {ncl_id, nch_id};
    }

    void send_packet_via_transport(const client_id &client_id_, const channel_id &channel_id_)
    {
        auto ch_it = channels.find(channel_id_);
        auto cl_it = client_sockaddr.find(client_id_);

        assert(ch_it != channels.end());
        assert(cl_it != client_sockaddr.end());

        std::unique_ptr<i_packet> pkt = ch_it->second->on_transport_send();

        serialize_common_header(pkt->get_buffer() + rudp_protocol::COMMON_HEADER_OFFSET, pkt->get_capacity() - rudp_protocol::COMMON_HEADER_OFFSET, rudp_protocol::common_header(client_id_, channel_id_));

        udp_transport->send_packet_to_network(*(cl_it->second), *pkt);
    }

    std::shared_ptr<rudp_protocol::session_control> session_control_;
    void handle_control_packet(const client_id &cl_id, std::unique_ptr<i_sockaddr> source_addr, const char *ibuf, const uint32_t &sz)
    {
        session_control_->handle_control_packet(cl_id, std::move(source_addr), ibuf, sz);
    }

public:
    channel_manager() : event_loop_thread(std::jthread([this](std::stop_token token)
                                                       { this->event_loop(std::move(token)); })) {}

    // for i_server
    channel_id add_channel(channel_type type) override
    {
        if (channels.size() >= MAX_CHANNELS)
            return INVALID_CHANNEL_ID;

        channel_id toret;
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<channel_id> distrib(1, MAX_CHANNELS - 1);

        while (true)
        {
            toret = distrib(gen);
            if (toret != INVALID_CHANNEL_ID && !channels.contains(toret))
            {
                switch (type)
                {
                case channel_type::RELIABLE_ORDERED_CHANNEL:
                    channels.emplace(toret, std::make_unique<reliable_ordered_channel>(toret));
                    break;
                default:
                    toret = INVALID_CHANNEL_ID;
                    break;
                }
                return toret;
            }
        }
    }
    ssize_t read_from_channel_nonblocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) override
    {
        auto result = ready_list->try_pop();
        if (!result.first)
            return -1;

        auto [cl_id, ch_id] = result.second;
        client_id_ = cl_id;
        channel_id_ = ch_id;

        auto &cur_channel = per_client_channels[client_id_][channel_id_];
        return cur_channel->read_bytes_to_application(buf, len);
    }
    ssize_t read_from_channel_blocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) override
    {
        auto [cl_id, ch_id] = ready_list->pop();

        client_id_ = cl_id;
        channel_id_ = ch_id;

        auto &cur_channel = per_client_channels[client_id_][channel_id_];
        return cur_channel->read_bytes_to_application(buf, len);
    }
    ssize_t write_to_channel(const channel_id &channel_id_, const client_id &client_id_, const char *buf, const size_t len) override
    {
        auto &cur_channel = per_client_channels[client_id_][channel_id_];
        return cur_channel->write_bytes_from_application(buf, len);
    }
    void start_server() override
    {
        udp_transport->start_io();
    }

    // for i_trasnsport
    void on_transport_receive(std::unique_ptr<i_packet> pkt, std::unique_ptr<i_sockaddr> source_addr) override
    {
        static rudp_protocol::common_header c_header;

        c_header = deserialize_common_header(pkt->get_buffer() + rudp_protocol::COMMON_HEADER_OFFSET, pkt->get_length() - rudp_protocol::COMMON_HEADER_OFFSET);

        if (c_header.ch_id == CONTROL_CHANNEL_ID)
        {
            handle_control_packet(c_header.cl_id, std::move(source_addr), pkt->get_buffer() + rudp_protocol::CHANNEL_HEADER_OFFSET, pkt->get_length() - rudp_protocol::CHANNEL_HEADER_OFFSET);
        }
        else if (channels.contains(c_header.ch_id))
        {
            auto addr_it = client_sockaddr.find(c_header.cl_id);
            if (addr_it != client_sockaddr.end() && compare_sockaddrs_content(*addr_it->second, *source_addr))
            {
                per_client_channels[c_header.cl_id][c_header.ch_id]->on_transport_receive(pkt->get_buffer() + rudp_protocol::CHANNEL_HEADER_OFFSET, pkt->get_length() - rudp_protocol::CHANNEL_HEADER_OFFSET);
            }
            // else client ID is coming from wrong source address, packet ignored.
        }
        // else channel does not exist, packet ignored.
    }

    // for i session control
    
    void add_client(const client_id &cl_id) override {}
    void remove_client(const client_id &cl_id) override {}
    void send_control_packet_via_transport(const i_sockaddr &dest_addr, const client_id &cl_id, std::unique_ptr<i_packet> pkt) override
    {
        serialize_common_header(pkt->get_buffer() + rudp_protocol::COMMON_HEADER_OFFSET, pkt->get_capacity() - rudp_protocol::COMMON_HEADER_OFFSET, rudp_protocol::common_header(cl_id, CONTROL_CHANNEL_ID));
        udp_transport->send_packet_to_network(dest_addr, *pkt);
    }

    // for creator
    void set_udp_transport(std::shared_ptr<udp> udp_)
    {
        udp_transport = udp_;
    }
    void set_session_control(std::shared_ptr<rudp_protocol::session_control> ses_control)
    {
        session_control_ = ses_control;
    }
};