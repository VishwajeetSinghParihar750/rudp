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
#include <chrono>

#include "types.hpp"
#include "thread_safe_unordered_set.hpp"
#include "i_channel.hpp"
#include "udp.hpp"
#include "rudp_protocol.hpp"
#include "channels/realible_ordered_channel/reliable_ordered_channel.hpp"
#include "i_server.hpp"
#include "i_udp_callback.hpp"
#include "i_session_control_callback.hpp"
#include "thread_safe_priority_queue.hpp"
#include "i_channel_manager_callback.hpp"
#include "timer_manager.hpp"

enum class READ_FROM_CHANNEL_ERROR : ssize_t
{
    CLIENT_DISCONNECTED = -1,
    NO_PENDING_DATA = -2

};
struct rcv_ready_queue_info
{
    std::chrono::steady_clock::time_point time;
    client_id cl_id;
    channel_id ch_id;

    bool operator<(const rcv_ready_queue_info &other) const
    {
        return time < other.time;
    }
};

class channel_manager : public i_server, public i_session_control_callback
{

    static constexpr uint32_t MAX_CHANNELS = 2048;

    std::shared_ptr<thread_safe_priority_queue<rcv_ready_queue_info, std::vector<rcv_ready_queue_info>, std::greater<rcv_ready_queue_info>>> ready_to_rcv_queue;

    std::shared_ptr<timer_manager> global_timers_manager;
    std::unordered_map<channel_id, channel_type> channels;
    std::unordered_map<client_id, std::unordered_map<channel_id, std::unique_ptr<i_channel>>> per_client_channels;

    thread_safe_unordered_set<client_id> pending_disconnects;

    void serialize_channel_manager_header(rudp_protocol_packet &pkt, const rudp_protocol::channel_manager_header &c_header)
    {
        char *buf = pkt.get_buffer();
        size_t len = pkt.get_length();
        // ⚠️ need to implement rdup protocol packet first then get correct offset

        assert(len >= rudp_protocol::CHANNEL_MANAGER_HEADER_SIZE);
        uint16_t nch_id = static_cast<uint16_t>(c_header.ch_id);
        nch_id = htons(nch_id);
        memcpy(buf, &nch_id, sizeof(uint16_t));
    }
    rudp_protocol::channel_manager_header deserialize_channel_manager_header(rudp_protocol_packet &pkt)
    {
        char *buf = pkt.get_buffer();
        size_t len = pkt.get_length();
        // ⚠️ need to implement rdup protocol packet first then get correct offset

        assert(len >= rudp_protocol::CHANNEL_MANAGER_HEADER_SIZE);
        uint16_t nch_id = *reinterpret_cast<const uint16_t *>(buf);
        nch_id = ntohs(nch_id);
        return {nch_id};
    }

    std::shared_ptr<i_channel_manager_callback> session_control_;

    std::unique_ptr<i_channel> create_new_channel_for_client(client_id cl_id, channel_id ch_id)
    {
        if (!channels.contains(ch_id))
            return nullptr;

        switch (channels[ch_id])
        {
        case channel_type::RELIABLE_ORDERED_CHANNEL:
        {
            auto ch = std::make_unique<reliable_ordered_channel>(ch_id);

            ch->set_timer_manager(global_timers_manager);

            ch->set_on_app_data_ready([this, cl_id, ch_id]()
                                      {
                rcv_ready_queue_info info;
                info.ch_id = ch_id;
                info.cl_id= cl_id;
                info.time = std::chrono::steady_clock::now();
            ready_to_rcv_queue->push(std::move(info)) ; });

            ch->set_on_net_data_ready([this, cl_id, ch_id](std::unique_ptr<rudp_protocol_packet> pkt)
                                      { on_transport_send(cl_id, ch_id, std::move(pkt)); });

            return ch;
        }

        default:
            return nullptr;
        }
    }

    void on_transport_send(const client_id &cl_id, const channel_id &ch_id, std::unique_ptr<rudp_protocol_packet> pkt)
    {
        serialize_channel_manager_header(*pkt, ch_id);
        session_control_->on_transport_send_data(cl_id, std::move(pkt));
    }

    void perform_final_cleanup(client_id cl_id)
    {
        per_client_channels.erase(cl_id);
        session_control_->notify_removal_of_client(cl_id);
    }

public:
    // for i_server
    void add_channel(channel_id ch_id, channel_type type) override
    {
        //  ℹ️add error handling to resopns back with error if wrong
        if (channels.size() >= MAX_CHANNELS)
            return;

        if (ch_id != INVALID_CHANNEL_ID && !channels.contains(ch_id))
        {
            channels.emplace(ch_id, type);
        }
    }

    void close_server() override
    {
        session_control_->on_close_server(); // now nothing should come to me from server, and if application tries to read or write after calling close, its undefined from my side
    }

    ssize_t read_from_channel_nonblocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) override
    {

        if (pending_disconnects.size() > 0)
        {
            client_id_ = pending_disconnects.pop();
            channel_id_ = INVALID_CHANNEL_ID;

            return (ssize_t)READ_FROM_CHANNEL_ERROR::CLIENT_DISCONNECTED;
        }

        rcv_ready_queue_info info;
        bool result = ready_to_rcv_queue->pop(info);
        if (!result)
            return (ssize_t)READ_FROM_CHANNEL_ERROR::NO_PENDING_DATA;

        client_id_ = info.cl_id;
        channel_id_ = info.ch_id;

        if (!per_client_channels.contains(client_id_) || !per_client_channels[client_id_].contains(channel_id_))
            return read_from_channel_nonblocking(channel_id_, client_id_, buf, len);

        auto &cur_channel = per_client_channels[client_id_][channel_id_];
        return cur_channel->read_bytes_to_application(buf, len);
    }

    ssize_t read_from_channel_blocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) override
    {
        rcv_ready_queue_info info;

        while (true)
        {
            if (pending_disconnects.size() > 0)
            {
                client_id_ = pending_disconnects.pop();
                channel_id_ = INVALID_CHANNEL_ID;

                return (ssize_t)READ_FROM_CHANNEL_ERROR::CLIENT_DISCONNECTED;
            }

            auto result = ready_to_rcv_queue->wait_for_and_pop(info, duration_ms(100));

            if (result)
            {
                client_id_ = info.cl_id;
                channel_id_ = info.ch_id;

                if (per_client_channels.contains(client_id_) && per_client_channels[client_id_].contains(channel_id_))
                {
                    auto &cur_channel = per_client_channels[client_id_][channel_id_];
                    return cur_channel->read_bytes_to_application(buf, len);
                }
            }
        }
    }
    ssize_t write_to_channel(const channel_id &channel_id_, const client_id &client_id_, const char *buf, const size_t len) override
    {
        if (!per_client_channels.contains(client_id_))
            return -1;

        auto &cur_channel = per_client_channels[client_id_][channel_id_];
        return cur_channel->write_bytes_from_application(buf, len);
    }

    // for i session control

    void on_transport_receive(const client_id &cl_id, std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (!per_client_channels.contains(cl_id))
            return;

        rudp_protocol::channel_manager_header cm_header = deserialize_channel_manager_header(*pkt);
        if (channels.contains(cm_header.ch_id))
            per_client_channels[cl_id][cm_header.ch_id]->on_transport_receive(std::move(pkt));
    }

    void add_client(const client_id &cl_id) override
    {
        per_client_channels.emplace(cl_id);
    }
    void remove_client(const client_id &cl_id) override
    {
        pending_disconnects.insert(cl_id);
    }
    void set_timer_manager(std::shared_ptr<timer_manager> timer_man) { global_timers_manager = timer_man; }
    void set_session_control(std::shared_ptr<i_channel_manager_callback> ses_control)
    {
        session_control_ = ses_control;
    }
};