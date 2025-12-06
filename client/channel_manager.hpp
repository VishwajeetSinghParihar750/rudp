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
#include "i_client.hpp"
#include "i_udp_callback.hpp"
#include "i_sockaddr.hpp"

class channel_manager : public i_client, public i_udp_callback
{
    static constexpr uint32_t MAX_CHANNELS = 2048;

    /*
    here channels add their own id when they are ready to give app stuff,
    they will have ptr to thread_safe_unordered_set
    channel manager will take out stuff
    */

    std::shared_ptr<thread_safe_unordered_set<std::pair<client_id, channel_id>>> ready_list;

    std::shared_ptr<udp> udp_transport;

    std::set<timer_info> timers;

    std::unordered_map<channel_id, std::unique_ptr<channel>> channels;

    std::unordered_map<client_id, std::unordered_map<channel_id, std::unique_ptr<channel>>> per_client_channels;

    std::unordered_map<client_id, std::shared_ptr<i_sockaddr>> client_sockaddr;

    void handle_timeout(timer_info timer_info_)
    {
        // ... logic to send retransmission packets, etc.
    }

    std::jthread event_loop_thread;

    void event_loop(std::stop_token token)
    {
        // ⚠️ here it might be sleeping more than it should, it should also wake if new timer is added to set
        while (!token.stop_requested())
        {
            auto it = timers.begin();
            if (it == timers.end())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (it->has_expired())
            {
                handle_timeout(std::move(*it));
                timers.erase(it);
            }
            else
            {
                std::this_thread::sleep_until(it->expiration_time);
            }
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

public:
    channel_manager() : event_loop_thread(std::jthread([this](std::stop_token token)
                                                       { this->event_loop(std::move(token)); })) {}

    void set_udp_transport(std::shared_ptr<udp> udp_)
    {
        udp_transport = udp_;
    }

    // for i_server
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

    // for i_trasnsport
    ssize_t on_transport_receive(std::unique_ptr<i_packet> pkt, std::unique_ptr<i_sockaddr> source_addr) override
    {
        static rudp_protocol::common_header c_header;

        c_header = deserialize_common_header(pkt->get_buffer() + rudp_protocol::COMMON_HEADER_OFFSET, pkt->get_length() - rudp_protocol::COMMON_HEADER_OFFSET);

        auto ch_it = channels.find(c_header.ch_id);

        if (ch_it != channels.end())
        {
            auto addr_it = client_sockaddr.find(c_header.cl_id);
            if (addr_it == client_sockaddr.end())
            {
                // maybe a new connection request
            }
            else
            {
                if (compare_sockaddrs_content(*addr_it->second, *source_addr))
                {
                    per_client_channels[c_header.cl_id][c_header.ch_id]->on_transport_receive(pkt->get_buffer() + rudp_protocol::CHANNEL_HEADER_OFFSET, pkt->get_length() - rudp_protocol::CHANNEL_HEADER_OFFSET);
                }
                // else client ID is coming from wrong source address, packet ignored.
            }
        }
        // else channel does not exist, packet ignored.
        return 0;
    }

    void connect() override
    {
        udp_transport->start_io();
    }
};