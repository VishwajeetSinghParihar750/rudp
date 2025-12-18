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
#include <sstream>

#include "../common/types.hpp"
#include "../common/thread_safe_unordered_map.hpp"
#include "../common/i_channel.hpp"
#include "../common/thread_safe_priority_queue.hpp"
#include "../common/i_timer_service.hpp"
#include "../common/logger.hpp"
#include "../common/rudp_protocol_packet.hpp"
#include "../common/channels/reliable_ordered_channel/reliable_ordered_channel.hpp"
#include "../common/channels/unordered_unreliable_channel/unordered_unreliable_channel.hpp"
#include "../common/channels/ordered_unreliable_channel/ordered_unreliable_channel.hpp"

#include "i_client.hpp"
#include "i_channel_manager_for_session_control.hpp"
#include "i_session_control_for_channel_manager.hpp"
#include "udp.hpp"

struct channel_manager_header
{
    channel_id ch_id;
    channel_manager_header(const channel_id &id) : ch_id(id) {}
};
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

    static constexpr uint32_t MAX_CHANNELS = 2048;

    thread_safe_priority_queue<rcv_ready_queue_info, std::vector<rcv_ready_queue_info>, std::greater<rcv_ready_queue_info>> ready_to_rcv_queue;

    std::shared_ptr<i_timer_service> global_timers_manager;
    std::unordered_map<channel_id, channel_type> channels;

    thread_safe_unordered_map<channel_id, std::shared_ptr<i_channel>> active_channels;

    std::shared_ptr<i_session_control_for_channel_manager> session_control_;

    std::atomic<bool> server_closed = false;
    void serialize_channel_manager_header(rudp_protocol_packet &pkt, const channel_id &ch_id)
    {

        const size_t off = rudp_protocol_packet::CHANNEL_MANAGER_HEADER_OFFSET;
        const size_t sz = rudp_protocol_packet::CHANNEL_MANAGER_HEADER_SIZE;

        if (pkt.get_length() < off + sz)
        {
            LOG_ERROR("Serialize: Packet capacity too small for header.");
        }
        assert(pkt.get_length() >= off + sz);

        uint32_t nch_id = static_cast<uint32_t>(ch_id);
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

    std::shared_ptr<i_channel> create_new_active_channel(channel_id ch_id)
    {
        auto ch_iter = channels.find(ch_id);
        if (ch_iter == channels.end())
        {
            LOG_WARN("Attempted to create active channel for unknown channel ID: " << ch_id);
            return nullptr;
        }

        switch (ch_iter->second)
        {
        case channel_type::RELIABLE_ORDERED_CHANNEL:
        {
            auto ch = std::make_shared<reliable_ordered_channel::reliable_ordered_channel>(ch_id);

            ch->set_timer_service(global_timers_manager);

            std::weak_ptr<channel_manager> this_weak_ptr = shared_from_this();
            ch->set_on_app_data_ready(
                [this_weak_ptr, ch_id]()
                {
                    if (auto sp = this_weak_ptr.lock())
                    {
                        rcv_ready_queue_info info;
                        info.ch_id = ch_id;
                        info.time = std::chrono::steady_clock::now();
                        sp->ready_to_rcv_queue.push(std::move(info));
                        LOG_INFO("Channel " << ch_id << " set to ready to receive.");
                    }
                });

            ch->set_on_net_data_ready(
                [this_weak_ptr, ch_id](std::unique_ptr<rudp_protocol_packet> pkt)
                {
                    if (auto sp = this_weak_ptr.lock())
                        sp->on_transport_send(ch_id, std::move(pkt));
                });

            active_channels.insert(ch_id, ch);
            LOG_INFO("Created new active reliable ordered channel with ID: " << ch_id);
            return ch;
        }

        case channel_type::UNORDERED_UNRELIABLE_CHANNEL:
        {
            auto ch = std::make_shared<unordered_unreliable_channel::unordered_unreliable_channel>(ch_id);

            ch->set_timer_service(global_timers_manager);

            std::weak_ptr<channel_manager> this_weak_ptr = shared_from_this();
            ch->set_on_app_data_ready(
                [this_weak_ptr, ch_id]()
                {
                    if (auto sp = this_weak_ptr.lock())
                    {
                        rcv_ready_queue_info info;
                        info.ch_id = ch_id;
                        info.time = std::chrono::steady_clock::now();
                        sp->ready_to_rcv_queue.push(std::move(info));
                        LOG_INFO("Channel " << ch_id << " set to ready to receive.");
                    }
                });

            ch->set_on_net_data_ready(
                [this_weak_ptr, ch_id](std::unique_ptr<rudp_protocol_packet> pkt)
                {
                    if (auto sp = this_weak_ptr.lock())
                        sp->on_transport_send(ch_id, std::move(pkt));
                });

            active_channels.insert(ch_id, ch);
            LOG_INFO("Created new active reliable ordered channel with ID: " << ch_id);
            return ch;
        }
        case channel_type::ORDERED_UNRELIABLE_CHANNEL:
        {
            auto ch = std::make_shared<ordered_unreliable_channel::ordered_unreliable_channel>(ch_id);

            ch->set_timer_service(global_timers_manager);

            std::weak_ptr<channel_manager> this_weak_ptr = shared_from_this();
            ch->set_on_app_data_ready(
                [this_weak_ptr, ch_id]()
                {
                    if (auto sp = this_weak_ptr.lock())
                    {
                        rcv_ready_queue_info info;
                        info.ch_id = ch_id;
                        info.time = std::chrono::steady_clock::now();
                        sp->ready_to_rcv_queue.push(std::move(info));
                        LOG_INFO("Channel " << ch_id << " set to ready to receive.");
                    }
                });

            ch->set_on_net_data_ready(
                [this_weak_ptr, ch_id](std::unique_ptr<rudp_protocol_packet> pkt)
                {
                    if (auto sp = this_weak_ptr.lock())
                        sp->on_transport_send(ch_id, std::move(pkt));
                });

            active_channels.insert(ch_id, ch);
            LOG_INFO("Created new active reliable ordered channel with ID: " << ch_id);
            return ch;
        }

        default:
        {
            LOG_ERROR("Attempted to create channel with unhandled type for ID: " << ch_id);
            return nullptr;
        }
        }
    }

    void on_transport_send(const channel_id &ch_id, std::unique_ptr<rudp_protocol_packet> pkt)
    {

        serialize_channel_manager_header(*pkt, ch_id);
        session_control_->on_transport_send_data(std::move(pkt));
        LOG_INFO("Forwarded packet from channel " << ch_id << " to session control for transport send.");
    }

    void set_timer_service(std::shared_ptr<i_timer_service> timer_man) { global_timers_manager = timer_man; }
    void set_session_control(std::shared_ptr<i_session_control_for_channel_manager> ses_control)
    {
        session_control_ = ses_control;
    }

public:
    class client_setup_access_key
    {
        friend std::shared_ptr<i_client> create_client(const char *, const char *);

    private:
        client_setup_access_key() {}
    };
    void add_channel(channel_id ch_id, channel_type type) override
    {
        if (channels.size() >= MAX_CHANNELS)
        {
            LOG_WARN("Cannot add channel " << ch_id << ". Maximum channel limit (" << MAX_CHANNELS << ") reached.");
            return;
        }

        if (ch_id != INVALID_CHANNEL_ID && !channels.contains(ch_id))
        {
            channels.emplace(ch_id, type);
            LOG_INFO("Channel " << ch_id << " of type " << static_cast<int>(type) << " added to available channels.");
        }
        else
        {
            LOG_WARN("Failed to add channel " << ch_id << ". It is either INVALID_CHANNEL_ID or already exists.");
        }
    }

    void close_client() override
    {
        LOG_INFO("Client closing initiated by application.");
        session_control_->on_close_client();
    }

    ssize_t read_from_channel_nonblocking(channel_id &channel_id_, char *buf, const size_t len) override
    {
        if (server_closed.load())
        {
            channel_id_ = INVALID_CHANNEL_ID;
            LOG_WARN("Attempted non-blocking read after server disconnected.");
            return (ssize_t)READ_FROM_CHANNEL_ERROR::SERVER_DISCONNECTED;
        }

        rcv_ready_queue_info info;
        bool result = ready_to_rcv_queue.pop(info);
        if (!result)
        {
            return (ssize_t)READ_FROM_CHANNEL_ERROR::NO_PENDING_DATA;
        }

        channel_id_ = info.ch_id;

        if (!active_channels.contains(channel_id_))
        {
            LOG_WARN("Channel " << channel_id_ << " was in ready queue but not in active channels. Trying next.");
            return read_from_channel_nonblocking(channel_id_, buf, len);
        }

        auto cur_channel_opt = active_channels.get(channel_id_);
        if (cur_channel_opt)
        {
            ssize_t bytes_read = cur_channel_opt.value()->read_bytes_to_application(buf, len);
            LOG_INFO("Non-blocking read on channel " << channel_id_ << " returned " << bytes_read << " bytes.");
            return bytes_read;
        }

        LOG_WARN("Channel " << channel_id_ << " was in ready queue but got removed from active channels before read. Trying next.");
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
                LOG_WARN("Attempted blocking read after server disconnected.");
                return (ssize_t)READ_FROM_CHANNEL_ERROR::SERVER_DISCONNECTED;
            }

            auto result = ready_to_rcv_queue.wait_for_and_pop(info, duration_ms(100));

            if (result)
            {
                channel_id_ = info.ch_id;

                auto ch_opt = active_channels.get(channel_id_);
                if (ch_opt)
                {
                    auto cur_channel = ch_opt.value();
                    ssize_t bytes_read = cur_channel->read_bytes_to_application(buf, len);
                    LOG_INFO("Blocking read on channel " << channel_id_ << " returned " << bytes_read << " bytes.");

                    if (bytes_read == 0)
                        return read_from_channel_blocking(channel_id_, buf, len);

                    return bytes_read;
                }
                else
                {
                    LOG_WARN("Blocking read: Channel " << channel_id_ << " found in ready queue but not in active channels. Resuming wait.");
                }
            }
        }
    }
    ssize_t write_to_channel(const channel_id &channel_id_, const char *buf, const size_t len) override
    {
        if (server_closed.load())
        {
            LOG_WARN("Attempted write on channel " << channel_id_ << " after server disconnected.");
            return -1;
        }
        auto ch_opt = active_channels.get(channel_id_);
        if (!ch_opt)
        {

            if (channels.contains(channel_id_))
            {
                LOG_INFO("Channel " << channel_id_ << " is known but not active. Attempting to create new active channel.");
                if (create_new_active_channel(channel_id_) != nullptr)
                {
                    return write_to_channel(channel_id_, buf, len);
                }
                else
                {
                    LOG_ERROR("Failed to create new active channel for ID: " << channel_id_);
                    return -1;
                }
            }
            else
            {
                LOG_ERROR("Attempted write on unknown channel ID: " << channel_id_);
                return -1;
            }
        }

        auto cur_channel = ch_opt.value();
        ssize_t bytes_written = cur_channel->write_bytes_from_application(buf, len);
        LOG_INFO("Write on active channel " << channel_id_ << " returned " << bytes_written << " bytes.");
        return bytes_written;
    }

    void on_server_disconnected() override
    {
        server_closed.store(true);
        LOG_CRITICAL("Server disconnected. Setting server_closed flag and notifying waiting threads.");
    }
    void on_transport_receive(std::unique_ptr<rudp_protocol_packet> pkt) override
    {
        if (pkt->get_length() < rudp_protocol_packet::CHANNEL_MANAGER_HEADER_OFFSET + rudp_protocol_packet::CHANNEL_MANAGER_HEADER_SIZE)
        {
            LOG_ERROR(" packet too small for channel manager ");
            return;
        }

        channel_manager_header cm_header = deserialize_channel_manager_header(*pkt);
        channel_id ch_id = cm_header.ch_id;

        auto ch_opt = active_channels.get(ch_id);

        if (ch_opt)
        {
            LOG_INFO("Received transport data for active channel " << ch_id << ".");
            ch_opt.value()->on_transport_receive(std::move(pkt));
        }
        else if (channels.contains(ch_id))
        {
            LOG_INFO("Received transport data for known, inactive channel " << ch_id << ". Attempting to activate and process.");
            if (create_new_active_channel(ch_id) != nullptr)
                on_transport_receive(std::move(pkt));
            else
            {
                LOG_ERROR("Failed to activate channel " << ch_id << " on first receive. Dropping packet.");
            }
        }
        else
        {
            LOG_WARN("Received transport data for unknown channel " << ch_id << ". Dropping packet.");
        }
    }

    void set_timer_service(std::shared_ptr<i_timer_service> timer_man, client_setup_access_key) { global_timers_manager = timer_man; }
    void set_session_control(std::shared_ptr<i_session_control_for_channel_manager> ses_control, client_setup_access_key)
    {
        session_control_ = ses_control;
    }
};