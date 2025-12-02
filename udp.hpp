#pragma once

#include <sys/socket.h>
#include <netdb.h>
#include <cassert>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory>
#include <thread>
#include <sys/epoll.h>

#include "types.hpp"
#include "channel_manager.hpp"
#include "i_sockaddr.hpp"
#include "raw_packet.hpp"

class udp
{
    std::shared_ptr<channel_manager> channel_manager_;

    int socket_fd = -1;
    int epoll_fd = -1;

    void set_non_blocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
    }

    void initialize_socket()
    {
        addrinfo hints{};
        addrinfo *results = nullptr;

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;

        int res = getaddrinfo(NULL, "1049", &hints, &results);
        assert(res == 0);

        for (addrinfo *p = results; p != nullptr; p = p->ai_next)
        {
            socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (socket_fd == -1)
                continue;

            set_non_blocking(socket_fd); //

            int optval = 1;
            int sopres = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
            assert(sopres != -1);

            int res_ = bind(socket_fd, p->ai_addr, p->ai_addrlen);
            if (res_ == 0)
            {
                break;
            }

            close(socket_fd);
            socket_fd = -1;
        }

        freeaddrinfo(results);

        assert(socket_fd != -1);
    }

    void setup_epoll()
    {
        epoll_event event;

        epoll_fd = epoll_create1(0);

        assert(epoll_fd != -1);

        event.data.fd = socket_fd;
        event.events = EPOLLIN;

        int epoll_ctl_rv = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event);

        assert(epoll_ctl_rv == 0);
    }

    void event_loop(std::stop_token token)
    {
        constexpr int MAX_EVENTS = 2048;
        epoll_event event, events[MAX_EVENTS];

        int event_cnt = 0;
        while (!token.stop_requested())
        {
            event_cnt = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

            for (auto ind = 0; ind < event_cnt; ind++)
            {
                //
                int current_fd = events[ind].data.fd;
                if (current_fd == socket_fd)
                {
                    std::unique_ptr<raw_packet> pkt = std::make_unique<raw_packet>(1500);
                    std::unique_ptr<i_sockaddr> addr = std::make_unique<i_sockaddr>();

                    int n = recvfrom(socket_fd, pkt->get_buffer(), pkt->get_capacity(), 0, addr->get_mutable_sockaddr(), addr->get_mutable_socklen());

                    if (n <= 0)
                    {
                        // handle error, assert for now
                        assert(n > 0);
                    }
                    else
                    {
                        pkt->set_length(n);
                        channel_manager_->on_transport_receive(std::move(pkt), std::move(addr));
                    }
                }
            }
        }
    }

public:
    udp()
    {
        initialize_socket();
        setup_epoll();
        std::jthread thread(event_loop); //
    }

    ~udp()
    {
        close(socket_fd);
        close(epoll_fd);
    }

    ssize_t send_packet_to_network(const i_sockaddr &addr, const i_packet &packet)
    {
        return sendto(
            socket_fd,
            packet.get_const_buffer(), packet.get_length(),
            0,
            addr.get_sockaddr(), addr.get_socklen());
    }
};