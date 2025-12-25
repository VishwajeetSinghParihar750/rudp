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

#include "../common/types.hpp"
#include "../common/thread_safe_unordered_map.hpp"
#include "../common/thread_safe_unordered_set.hpp"
#include "../common/i_channel.hpp"
#include "../common/channels/reliable_ordered_channel/reliable_ordered_channel.hpp"
#include "../common/channels/ordered_unreliable_channel/ordered_unreliable_channel.hpp"
#include "../common/channels/unordered_unreliable_channel/unordered_unreliable_channel.hpp"
#include "../common/thread_safe_priority_queue.hpp"
#include "../common/i_timer_service.hpp"
#include "../common/logger.hpp"

#include "udp.hpp"
#include "i_session_control_for_channel_manager.hpp"
#include "i_server.hpp"
#include "i_channel_manager_for_session_control.hpp"

enum class READ_FROM_CHANNEL_ERROR : ssize_t
{
    CLIENT_DISCONNECTED = -1,
    NO_PENDING_DATA = -2

};

struct channel_manager_header
{
    channel_id ch_id;
    channel_manager_header(const channel_id &id) : ch_id(id) {}
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

    bool operator>(const rcv_ready_queue_info &other) const
    {
        return time > other.time;
    }
};

class channel_manager : public i_server, public i_channel_manager_for_session_control, public std::enable_shared_from_this<channel_manager>
{

    static constexpr uint32_t MAX_CHANNELS = 2048;

    thread_safe_priority_queue<rcv_ready_queue_info, std::vector<rcv_ready_queue_info>, std::greater<rcv_ready_queue_info>> ready_to_rcv_queue;

    std::shared_ptr<i_timer_service> global_timers_manager;
    std::unordered_map<channel_id, channel_type> channels;
    /*
        its undefined behavior to add channels after you have started the server
        so only read happens after server start, so its thread safe
    */

    using channel_map_t = thread_safe_unordered_map<channel_id, std::shared_ptr<i_channel>>;
    thread_safe_unordered_map<client_id, std::shared_ptr<channel_map_t>> per_client_channels;

    thread_safe_unordered_set<client_id> pending_disconnects;

    void serialize_channel_manager_header(rudp_protocol_packet &pkt, const channel_manager_header &c_header)
    {
        const size_t off = rudp_protocol_packet::CHANNEL_MANAGER_HEADER_OFFSET;
        const size_t sz = rudp_protocol_packet::CHANNEL_MANAGER_HEADER_SIZE;

        if (pkt.get_length() < off + sz)
        {
            LOG_ERROR("Serialize: Packet capacity too small for header.");
        }
        assert(pkt.get_length() >= off + sz);

        uint32_t nch_id = static_cast<uint32_t>(c_header.ch_id);
        uint32_t net = htonl(nch_id);
        memcpy(pkt.get_buffer() + off, &net, sizeof(net));
    }

    channel_manager_header deserialize_channel_manager_header(rudp_protocol_packet &pkt)
    {
        const size_t off = rudp_protocol_packet::CHANNEL_MANAGER_HEADER_OFFSET;
        const size_t sz = rudp_protocol_packet::CHANNEL_MANAGER_HEADER_SIZE;

        if (pkt.get_length() < off + sz)
        {
            LOG_ERROR("Deserialize: Packet length smaller than expected header size.");
        }
        assert(pkt.get_length() >= off + sz);

        const char *buf = pkt.get_const_buffer();
        uint32_t net = 0;
        memcpy(&net, buf + off, sizeof(net));
        uint32_t nch_id = ntohl(net);
        return {static_cast<channel_id>(nch_id)};
    }

    std::shared_ptr<i_session_control_for_channel_manager> session_control_;

    std::shared_ptr<i_channel> create_new_channel_for_client(client_id cl_id, channel_id ch_id)
    {
        LOG_INFO("Creating new channel " << ch_id << " for client " << cl_id);

        if (!channels.contains(ch_id))
        {
            LOG_ERROR("Attempted to create channel with unknown ID: " << ch_id);
            return nullptr;
        }

        switch (channels[ch_id])
        {
        case channel_type::RELIABLE_ORDERED_CHANNEL:
        {
            auto ch = std::make_shared<reliable_ordered_channel::reliable_ordered_channel>(ch_id);

            ch->set_timer_service(global_timers_manager);

            std::weak_ptr<channel_manager> this_weak_ptr = shared_from_this();
            ch->set_on_app_data_ready(
                [this_weak_ptr, cl_id, ch_id]()
                {
                    if (auto sp = this_weak_ptr.lock())
                    {
                        rcv_ready_queue_info info;
                        info.ch_id = ch_id;
                        info.cl_id = cl_id;
                        info.time = std::chrono::steady_clock::now();
                        sp->ready_to_rcv_queue.push(std::move(info));
                        LOG_TEST("RCV Ready: Data from ch " << ch_id << " pushed to application queue.");
                    }
                });

            ch->set_on_net_data_ready(
                [this_weak_ptr, cl_id, ch_id](std::shared_ptr<rudp_protocol_packet> pkt)
                {
                    if (auto sp = this_weak_ptr.lock())
                        sp->on_transport_send(cl_id, ch_id, std::move(pkt));
                });

            auto opt = per_client_channels.get(cl_id);
            if (opt)
                opt.value()->insert(ch_id, ch);
            return ch;
        }
        case channel_type::UNORDERED_UNRELIABLE_CHANNEL:
        {
            auto ch = std::make_shared<unordered_unreliable_channel::unordered_unreliable_channel>(ch_id);

            ch->set_timer_service(global_timers_manager);

            std::weak_ptr<channel_manager> this_weak_ptr = shared_from_this();
            ch->set_on_app_data_ready(
                [this_weak_ptr, cl_id, ch_id]()
                {
                    if (auto sp = this_weak_ptr.lock())
                    {
                        rcv_ready_queue_info info;
                        info.ch_id = ch_id;
                        info.cl_id = cl_id;
                        info.time = std::chrono::steady_clock::now();
                        sp->ready_to_rcv_queue.push(std::move(info));
                        LOG_TEST("RCV Ready: Data from ch " << ch_id << " pushed to application queue.");
                    }
                });

            ch->set_on_net_data_ready(
                [this_weak_ptr, cl_id, ch_id](std::shared_ptr<rudp_protocol_packet> pkt)
                {
                    if (auto sp = this_weak_ptr.lock())
                        sp->on_transport_send(cl_id, ch_id, std::move(pkt));
                });

            auto opt = per_client_channels.get(cl_id);
            if (opt)
                opt.value()->insert(ch_id, ch);
            return ch;
        }
        case channel_type::ORDERED_UNRELIABLE_CHANNEL:
        {
            auto ch = std::make_shared<ordered_unreliable_channel::ordered_unreliable_channel>(ch_id);

            ch->set_timer_service(global_timers_manager);

            std::weak_ptr<channel_manager> this_weak_ptr = shared_from_this();
            ch->set_on_app_data_ready(
                [this_weak_ptr, cl_id, ch_id]()
                {
                    if (auto sp = this_weak_ptr.lock())
                    {
                        rcv_ready_queue_info info;
                        info.ch_id = ch_id;
                        info.cl_id = cl_id;
                        info.time = std::chrono::steady_clock::now();
                        sp->ready_to_rcv_queue.push(std::move(info));
                        LOG_TEST("RCV Ready: Data from ch " << ch_id << " pushed to application queue.");
                    }
                });

            ch->set_on_net_data_ready(
                [this_weak_ptr, cl_id, ch_id](std::shared_ptr<rudp_protocol_packet> pkt)
                {
                    if (auto sp = this_weak_ptr.lock())
                        sp->on_transport_send(cl_id, ch_id, std::move(pkt));
                });

            auto opt = per_client_channels.get(cl_id);
            if (opt)
                opt.value()->insert(ch_id, ch);
            return ch;
        }

        default:
            LOG_ERROR("Unsupported channel type for channel ID: " << ch_id);
            return nullptr;
        }
    }

    void on_transport_send(const client_id &cl_id, const channel_id &ch_id, std::shared_ptr<rudp_protocol_packet> pkt)
    {
        LOG_TEST("Transport Send: Client " << cl_id << ", Channel " << ch_id << ", Length " << pkt->get_length());

        serialize_channel_manager_header(*pkt, {ch_id});
        session_control_->on_transport_send_data(cl_id, std::move(pkt));
    }

    void perform_final_cleanup(client_id cl_id)
    {
        LOG_INFO("Final cleanup initiated for client " << cl_id);

        per_client_channels.erase(cl_id);
        session_control_->notify_removal_of_client(cl_id);
    }

public:
    channel_manager() = default;
    ~channel_manager() = default;
    // explictly deleting copy/move
    channel_manager(const channel_manager &) = delete;
    channel_manager &operator=(const channel_manager &) = delete;
    channel_manager(channel_manager &&) = delete;
    channel_manager &operator=(channel_manager &&) = delete;

    // selective access
    class server_setup_access_key
    {
        friend std::shared_ptr<i_server> create_server(const char *);

    private:
        server_setup_access_key() {}
    };
    //
    // for i_server
    void add_channel(channel_id ch_id, channel_type type) override
    {
        // ℹ️add error handling to resopns back with error if wrong
        if (channels.size() >= MAX_CHANNELS)
        {
            LOG_ERROR("Failed to add channel. MAX_CHANNELS limit reached: " << MAX_CHANNELS);
            return;
        }

        if (ch_id != INVALID_CHANNEL_ID && !channels.contains(ch_id))
        {
            channels.emplace(ch_id, type);
            LOG_INFO("Channel " << ch_id << " of type " << (int)type << " added.");
        }
        else if (ch_id == INVALID_CHANNEL_ID)
        {
            LOG_ERROR("Attempted to add channel with INVALID_CHANNEL_ID.");
        }
        else
        {
            LOG_ERROR("Channel " << ch_id << " already exists.");
        }
    }

    void close_server() override
    {
        LOG_INFO("Server close requested by application.");
        session_control_->on_close_server(); // now nothing should come to me from server, and if application tries to read or write after calling close, its undefined from my side
        // my things will get remvoed in destructor iteslf
    }

    ssize_t read_from_channel_nonblocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) override
    {
        LOG_TEST("Non-blocking read initiated.");

        if (auto disconnected_client = pending_disconnects.try_pop())
        {
            client_id_ = *disconnected_client;
            channel_id_ = INVALID_CHANNEL_ID;
            LOG_INFO("Non-blocking read returning CLIENT_DISCONNECTED for client " << client_id_);
            return (ssize_t)READ_FROM_CHANNEL_ERROR::CLIENT_DISCONNECTED;
        }

        rcv_ready_queue_info info;
        bool result = ready_to_rcv_queue.pop(info);
        if (!result)
        {
            LOG_TEST("Non-blocking read returning NO_PENDING_DATA.");
            return (ssize_t)READ_FROM_CHANNEL_ERROR::NO_PENDING_DATA;
        }
        LOG_TEST("Non-blocking read popped data for client " << info.cl_id << " ch " << info.ch_id);

        client_id_ = info.cl_id;
        channel_id_ = info.ch_id;

        auto client_map_opt = per_client_channels.get(client_id_);
        if (!client_map_opt)
        {
            LOG_ERROR("Non-blocking read: Client map missing for client " << client_id_);
            return read_from_channel_nonblocking(channel_id_, client_id_, buf, len);
        }

        auto client_map = client_map_opt.value();
        if (!client_map->contains(channel_id_))
        {
            LOG_ERROR("Non-blocking read: Channel map missing for ch " << channel_id_);
            return read_from_channel_nonblocking(channel_id_, client_id_, buf, len);
        }

        auto ch_opt = client_map->get(channel_id_);
        if (!ch_opt)
        {
            LOG_ERROR("Non-blocking read: Channel object missing for ch " << channel_id_);
            return read_from_channel_nonblocking(channel_id_, client_id_, buf, len);
        }

        auto cur_channel = ch_opt.value();
        ssize_t bytes_read = cur_channel->read_bytes_to_application(buf, len);
        LOG_INFO("Non-blocking read successful: " << bytes_read << " bytes from ch " << channel_id_);
        return bytes_read;
    }

    ssize_t read_from_channel_blocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) override
    {
        LOG_TEST("Blocking read initiated. Entering loop.");
        rcv_ready_queue_info info;

        while (true)
        {
            if (auto disconnected_client = pending_disconnects.try_pop())
            {
                client_id_ = *disconnected_client;
                channel_id_ = INVALID_CHANNEL_ID;
                LOG_INFO("Non-blocking read returning CLIENT_DISCONNECTED for client " << client_id_);
                return (ssize_t)READ_FROM_CHANNEL_ERROR::CLIENT_DISCONNECTED;
            }

            auto result = ready_to_rcv_queue.wait_for_and_pop(info, duration_ms(100));

            if (result)
            {
                client_id_ = info.cl_id;
                channel_id_ = info.ch_id;

                LOG_TEST("Blocking read succeeded for client " << client_id_ << " ch " << channel_id_);

                auto client_map_opt = per_client_channels.get(client_id_);
                if (client_map_opt)
                {
                    auto client_map = client_map_opt.value();
                    if (client_map->contains(channel_id_))
                    {
                        auto ch_opt = client_map->get(channel_id_);
                        if (ch_opt)
                        {
                            auto cur_channel = ch_opt.value();
                            ssize_t bytes_read = cur_channel->read_bytes_to_application(buf, len);
                            LOG_INFO("Blocking read successful: " << bytes_read << " bytes from ch " << channel_id_);

                            return bytes_read;
                        }
                    }
                }
                LOG_ERROR("Blocking read failed lookup for client " << client_id_ << " ch " << channel_id_ << ". Data dropped.");
            }
        }
    }

    ssize_t write_to_channel(const channel_id &channel_id_, const client_id &client_id_, const char *buf, const size_t len) override
    {
        LOG_TEST("Write to channel initiated: client " << client_id_ << " ch " << channel_id_ << " len " << len);

        auto client_map_opt = per_client_channels.get(client_id_);
        if (!client_map_opt)
        {
            LOG_ERROR("Write to channel failed: Client map missing for client " << client_id_);
            return -1;
        }

        auto client_map = client_map_opt.value();

        auto ch_opt = client_map->get(channel_id_);
        if (!ch_opt)
        {
            LOG_ERROR("Write to channel failed: Channel object missing for ch " << channel_id_);
            return -1;
        }

        auto cur_channel = ch_opt.value();
        ssize_t bytes_written = cur_channel->write_bytes_from_application(buf, len);
        LOG_INFO("Write to channel complete: " << bytes_written << " bytes to ch " << channel_id_);
        return bytes_written;
    }

    // for i session control

    void on_transport_receive(const client_id &cl_id, std::unique_ptr<rudp_protocol_packet> pkt) override
    {

        LOG_TEST("Transport Receive: Client " << cl_id << ", Length " << pkt->get_length());

        auto client_map_opt = per_client_channels.get(cl_id);
        if (!client_map_opt)
        {
            LOG_ERROR("Transport receive: Client map missing for client " << cl_id << ". Packet dropped.");
            return;
        }

        if (pkt->get_length() < rudp_protocol_packet::CHANNEL_MANAGER_HEADER_OFFSET + rudp_protocol_packet::CHANNEL_MANAGER_HEADER_SIZE)
        {
            LOG_INFO(" packet too small for channel manager, most likely control packet  ");
            return;
        }

        channel_manager_header cm_header = deserialize_channel_manager_header(*pkt);
        if (channels.contains(cm_header.ch_id))
        {
            auto client_map = client_map_opt.value();
            auto ch_opt = client_map->get(cm_header.ch_id);
            if (ch_opt)
            {
                LOG_TEST("Transport receive: Forwarding packet to ch " << cm_header.ch_id);
                ch_opt.value()->on_transport_receive(std::move(pkt));
            }
            else
            {
                auto res = create_new_channel_for_client(cl_id, cm_header.ch_id);
                if (res != nullptr)
                    on_transport_receive(cl_id, std::move(pkt));
                else
                    LOG_ERROR("Transport receive: Channel object missing for ch " << cm_header.ch_id << ". Packet dropped.");
            }
        }
        else
        {
            LOG_ERROR("Transport receive: Unknown channel ID " << cm_header.ch_id << ". Packet dropped.");
        }
    }

    void add_client(const client_id &cl_id) override
    {
        LOG_INFO("Adding new client: " << cl_id);
        // create a new thread-safe channel map for this client
        per_client_channels.insert(cl_id, std::make_shared<channel_map_t>());
    }
    void remove_client(const client_id &cl_id) override
    {
        LOG_INFO("Client requested for removal: " << cl_id);
        pending_disconnects.insert(cl_id);
    }

    void set_timer_service(std::shared_ptr<i_timer_service> timer_man, server_setup_access_key)
    {
        global_timers_manager = timer_man;
        LOG_INFO("Timer manager set.");
    }
    void set_session_control(std::shared_ptr<i_session_control_for_channel_manager> ses_control, server_setup_access_key)
    {
        session_control_ = ses_control;
        LOG_INFO("Session control set.");
    }
};