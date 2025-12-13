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
#include <iostream>
#include <sys/socket.h>

#include "types.hpp"
#include "raw_packet.hpp"
#include "i_udp_callback.hpp"
#include "i_sockaddr.hpp"
#include "wrapper_sockaddr.hpp"

class udp
{
    std::weak_ptr<i_udp_callback> channel_manager_;

    int socket_fd = -1;
    int epoll_fd = -1;
    std::jthread io_thread;

    std::string server_host, server_port;

    void set_non_blocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
    }

    void connect_socket(const char *HOST, const char *PORT)
    {
        addrinfo hints{};
        addrinfo *results = nullptr;

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = 0;

        int res = getaddrinfo(NULL, PORT, &hints, &results);
        assert(res == 0);

        for (addrinfo *p = results; p != nullptr; p = p->ai_next)
        {
            socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (socket_fd == -1)
                continue;

            set_non_blocking(socket_fd); //

            if (::connect(socket_fd, p->ai_addr, p->ai_addrlen) == 0)
            {
                set_non_blocking(socket_fd);
                break;
            }

            close(socket_fd);
            socket_fd = -1;
        }

        freeaddrinfo(results);

        assert(socket_fd != -1);

        server_host = HOST, server_port = PORT;
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
        std::cout << "UDP Client connected to " << server_host << ":" << server_port << " and ready to receive." << std::endl;

        constexpr int MAX_EVENTS = 2048;
        epoll_event event, events[MAX_EVENTS];
        std::shared_ptr<i_udp_callback> channel_manager_sp;
        constexpr int EPOLL_TIMEOUT_MS = 100;

        int event_cnt = 0;
        while (!token.stop_requested())
        {
            int event_cnt = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);

            if (token.stop_requested())
                break;

            if (event_cnt == -1)
            {
                if (errno != EINTR)
                {
                    std::cerr << "Client epoll_wait error: " << strerror(errno) << std::endl;
                }
                continue;
            }

            if (event_cnt > 0 && events[0].events & EPOLLIN)
            {

                std::unique_ptr<raw_packet> pkt = std::make_unique<raw_packet>(1500);
                std::unique_ptr<i_sockaddr> addr = std::make_unique<wrapper_sockaddr>();

                int n = recvfrom(socket_fd, pkt->get_buffer(), pkt->get_capacity(), 0, addr->get_mutable_sockaddr(), addr->get_mutable_socklen());

                if (n <= 0)
                {
                    // handle error, assert for now
                    assert(n > 0);
                }
                else
                {
                    pkt->set_length(n);
                    channel_manager_sp = channel_manager_.lock();
                    if (channel_manager_sp != nullptr)
                        channel_manager_sp->on_transport_receive(std::move(pkt), std::move(addr));
                    else
                        return;
                }
            }
        }
    }

public:
    udp(const char *host, const char *port) // like "4004"
    {
        connect_socket(host, port);
        setup_epoll();
    }

    void start_io()
    {
        io_thread = std::jthread([this](std::stop_token token)
                                 { this->event_loop(std::move(token)); });
    }

    ~udp()
    {
        std::cout << "connection closed " << std::endl;

        close(socket_fd);
        close(epoll_fd);

        io_thread.request_stop();
        io_thread.join();
    }

    ssize_t send_packet_to_network(const i_sockaddr &addr, const i_packet &packet)
    {
        return sendto(
            socket_fd,
            packet.get_const_buffer(), packet.get_length(),
            0,
            addr.get_sockaddr(), addr.get_socklen());
    }

    void set_channel_manager(std::weak_ptr<i_udp_callback> cm)
    {
        channel_manager_ = cm;
    }
};