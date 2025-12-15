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
#include "thread_safe_unordered_map.hpp"
#include "thread_safe_unordered_set.hpp"
#include "i_channel.hpp"
#include "udp.hpp"
#include "rudp_protocol.hpp"
#include "channels/realible_ordered_channel/reliable_ordered_channel.hpp"
#include "i_client.hpp"
#include "i_channel_manager_for_session_control.hpp"
#include "thread_safe_priority_queue.hpp"
#include "i_session_control_for_channel_manager.hpp"
#include "timer_manager.hpp"

enum class READ_FROM_CHANNEL_ERROR : ssize_t
{
    SERVER_DISCONNECTED = -1,
    NO_PENDING_DATA = -2

};
struct rcv_ready_queue_info
{
    std::chrono::steady_clock::time_point time;
    channel_id ch_id;

    bool operator<(const rcv_ready_queue_info &other) const
    {
        return time < other.time;
    }

    bool operator>(const rcv_ready_queue_info &other) const
    {
        return time > other.time;
    }
};

class channel_manager : public i_client, public i_channel_manager_for_session_control, public std::enable_shared_from_this<channel_manager>
{

    //

    static constexpr uint32_t MAX_CHANNELS = 2048;

    std::shared_ptr<thread_safe_priority_queue<rcv_ready_queue_info, std::vector<rcv_ready_queue_info>, std::greater<rcv_ready_queue_info>>> ready_to_rcv_queue;

    std::shared_ptr<timer_manager> global_timers_manager;
    std::unordered_map<channel_id, channel_type> channels;
    /*
        its undefined behavior to add channels after you have started the server
        so only read happens after server start, so its thread safe
    */

    thread_safe_unordered_map<channel_id, std::shared_ptr<i_channel>> active_channels;

    std::shared_ptr<i_session_control_for_channel_manager> session_control_;

    std::atomic<bool> server_closed = false;

    void serialize_channel_manager_header(rudp_protocol_packet &pkt, const rudp_protocol::channel_manager_header &c_header)
    {
        const size_t off = rudp_protocol::CHANNEL_MANAGER_HEADER_OFFSET;
        const size_t sz = rudp_protocol::CHANNEL_MANAGER_HEADER_SIZE;

        assert(pkt.get_capacity() < off + sz);

        uint32_t nch_id = static_cast<uint32_t>(c_header.ch_id);
        uint32_t net = htonl(nch_id);
        memcpy(pkt.get_buffer() + off, &net, sizeof(net));

        if (pkt.get_length() < off + sz)
            pkt.set_length(off + sz);
    }

    rudp_protocol::channel_manager_header deserialize_channel_manager_header(rudp_protocol_packet &pkt)
    {
        const size_t off = rudp_protocol::CHANNEL_MANAGER_HEADER_OFFSET;
        const size_t sz = rudp_protocol::CHANNEL_MANAGER_HEADER_SIZE;

        assert(pkt.get_length() >= off + sz);

        const char *buf = pkt.get_const_buffer();
        uint32_t net = 0;
        memcpy(&net, buf + off, sizeof(net));
        uint32_t nch_id = ntohl(net);
        return {static_cast<channel_id>(nch_id)};
    }

    std::shared_ptr<i_channel> create_new_active_channel(channel_id ch_id)
    {
        if (!channels.contains(ch_id))
            return nullptr;

        switch (channels[ch_id])
        {
        case channel_type::RELIABLE_ORDERED_CHANNEL:
        {
            auto ch = std::make_shared<reliable_ordered_channel>(ch_id);

            ch->set_timer_manager(global_timers_manager);

            std::weak_ptr<channel_manager> this_weak_ptr = shared_from_this();
            ch->set_on_app_data_ready(
                [this_weak_ptr, ch_id]()
                {
                    if (auto sp = this_weak_ptr.lock())
                    {
                        rcv_ready_queue_info info;
                        info.ch_id = ch_id;
                        info.time = std::chrono::steady_clock::now();
                        sp->ready_to_rcv_queue->push(std::move(info));
                    }
                });

            ch->set_on_net_data_ready(
                [this_weak_ptr, ch_id](std::unique_ptr<rudp_protocol_packet> pkt)
                {
                    if (auto sp = this_weak_ptr.lock())
                        sp->on_transport_send(ch_id, std::move(pkt));
                });

            return ch;
        }

        default:
            return nullptr;
        }
    }

    void on_transport_send(const channel_id &ch_id, std::unique_ptr<rudp_protocol_packet> pkt)
    {
        serialize_channel_manager_header(*pkt, ch_id);
        session_control_->on_transport_send_data(std::move(pkt));
    }

    friend auto create_server(const char *);

    void set_timer_manager(std::shared_ptr<timer_manager> timer_man) { global_timers_manager = timer_man; }
    void set_session_control(std::shared_ptr<i_session_control_for_channel_manager> ses_control)
    {
        session_control_ = ses_control;
    }

public:
    // selective access
    class client_setup_access_key
    {
        friend std::shared_ptr<i_client> create_client(const char *, const char *);

    private:
        client_setup_access_key() {}
    };
    // for i_client
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

    void close_client() override
    {
        session_control_->on_close_client(); // now nothing should come to me from server, and if application tries to read or write after calling close, its undefined from my side
        // my things will get remvoed in destructor iteslf
    }

    ssize_t read_from_channel_nonblocking(channel_id &channel_id_, char *buf, const size_t len) override
    {

        if (server_closed.load())
        {
            channel_id_ = INVALID_CHANNEL_ID;
            return (ssize_t)READ_FROM_CHANNEL_ERROR::SERVER_DISCONNECTED;
        }

        rcv_ready_queue_info info;
        bool result = ready_to_rcv_queue->pop(info);
        if (!result)
            return (ssize_t)READ_FROM_CHANNEL_ERROR::NO_PENDING_DATA;

        channel_id_ = info.ch_id;

        if (!active_channels.contains(channel_id_))
            return read_from_channel_nonblocking(channel_id_, buf, len);

        auto cur_channel_opt = active_channels.get(channel_id_);
        if (cur_channel_opt)
            return cur_channel_opt.value()->read_bytes_to_application(buf, len);

        return read_from_channel_nonblocking(channel_id_, buf, len);
    }

    ssize_t read_from_channel_blocking(channel_id &channel_id_, char *buf, const size_t len) override
    {
        rcv_ready_queue_info info;

        while (true)
        {
            if (server_closed.load())
            {
                channel_id_ = INVALID_CHANNEL_ID;
                return (ssize_t)READ_FROM_CHANNEL_ERROR::SERVER_DISCONNECTED;
            }

            auto result = ready_to_rcv_queue->wait_for_and_pop(info, duration_ms(100));

            if (result)
            {
                channel_id_ = info.ch_id;

                auto ch_opt = active_channels.get(channel_id_);
                if (ch_opt)
                {
                    auto cur_channel = ch_opt.value();
                    return cur_channel->read_bytes_to_application(buf, len);
                }
            }
        }
    }
    ssize_t write_to_channel(const channel_id &channel_id_, const char *buf, const size_t len) override
    {
        auto ch_opt = active_channels.get(channel_id_);
        if (!ch_opt)
        {
            if (channels.contains(channel_id_))
            {
                if (create_new_active_channel(channel_id_) != nullptr)
                    return write_to_channel(channel_id_, buf, len);
                else
                    return -1;
            }
            else
                return -1;
        }

        auto cur_channel = ch_opt.value();
        return cur_channel->write_bytes_from_application(buf, len);
    }

    // for i session control

    void on_server_disconnected() override
    {
        server_closed.store(true);
    }
    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        rudp_protocol::channel_manager_header cm_header = deserialize_channel_manager_header(*pkt);
        auto ch_opt = active_channels.get(cm_header.ch_id);

        if (ch_opt)
            ch_opt.value()->on_transport_receive(std::move(pkt));
        else if (channels.contains(cm_header.ch_id) && create_new_active_channel(cm_header.ch_id) != nullptr)
            on_transport_receive(std::move(pkt));
    }

    void set_timer_manager(std::shared_ptr<timer_manager> timer_man, client_setup_access_key) { global_timers_manager = timer_man; }
    void set_session_control(std::shared_ptr<i_session_control_for_channel_manager> ses_control, client_setup_access_key)
    {
        session_control_ = ses_control;
    }
};