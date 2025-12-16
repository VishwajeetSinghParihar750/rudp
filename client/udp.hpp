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
#include "sys/ioctl.h"

#include "i_session_control_for_udp.hpp"
#include "common/rudp_protocol_packet.hpp"
#include "transport_addr.hpp"
#include "i_udp_for_session_control.hpp"

class i_client;

class udp : public i_udp_for_session_control
{
    std::weak_ptr<i_session_control_for_udp> session_control_;

    int socket_fd = -1;
    int epoll_fd = -1;

    std::jthread io_thread;

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

        int res = getaddrinfo(HOST, PORT, &hints, &results);
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
        std::shared_ptr<i_session_control_for_udp> session_control_sp;

        int event_cnt = 0;
        while (!token.stop_requested())
        {
            event_cnt = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);

            if (token.stop_requested())
                break;

            if (event_cnt == -1)
            {
                if (errno != EINTR)
                {
                    std::cerr << "server epoll_wait error: " << strerror(errno) << std::endl;
                }
                continue;
            }

            for (auto ind = 0; ind < event_cnt; ind++)
            {
                //
                int current_fd = events[ind].data.fd;
                if (current_fd == socket_fd)
                {
                    //

                    size_t bytes_avail = 0;

                    ioctl(socket_fd, FIONREAD, &bytes_avail);

                    std::unique_ptr<rudp_protocol_packet> pkt = std::make_unique<rudp_protocol_packet>(bytes_avail);

                    int n = recv(socket_fd, pkt->get_buffer(), pkt->get_capacity(), 0);

                    if (n > 0)
                    {
                        pkt->set_length(n);
                        session_control_sp = session_control_.lock();
                        if (session_control_sp != nullptr)
                            session_control_sp->on_transport_receive(std::move(pkt));
                        else
                            return;
                    }
                }
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

    udp(const char *host, const char *port) // like "4004"
    {
        connect_socket(host, port);
        setup_epoll();
        io_thread = std::jthread([this](std::stop_token token)
                                 { this->event_loop(std::move(token)); });
    }

    ~udp()
    {
        std::cout << "connection closed " << std::endl;

        close(socket_fd);
        close(epoll_fd);
    }

    ssize_t send_packet_to_network(const char *buf, const size_t &len) override
    {
        return send(
            socket_fd,
            buf, len,
            0);
    }

    void set_sesion_control(std::weak_ptr<i_session_control_for_udp> cm, client_setup_access_key)
    {
        session_control_ = cm;
    }
};