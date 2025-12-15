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
#include <sys/ioctl.h>

#include "rudp_protocol_packet.hpp"
#include "i_udp_for_session_control.hpp"
#include "i_session_control_for_udp.hpp"
#include "transport_addr.hpp"

class i_server;

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

    void initialize_socket(const char *PORT)
    {
        addrinfo hints{};
        addrinfo *results = nullptr;

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;

        int res = getaddrinfo(NULL, PORT, &hints, &results);
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
        print_listener_info();

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
                    std::unique_ptr<transport_addr> addr = std::make_unique<transport_addr>();

                    int n = recvfrom(socket_fd, pkt->get_buffer(), pkt->get_capacity(), 0, addr->get_mutable_sockaddr(), addr->get_mutable_socklen());

                    if (n > 0)
                    {
                        pkt->set_length(n);
                        session_control_sp = session_control_.lock();
                        if (session_control_sp != nullptr)
                            session_control_sp->on_transport_receive(std::move(pkt), std::move(addr));
                        else
                            return;
                    }
                }
            }
        }
    }

    void print_listener_info()
    {
        assert(socket_fd != -1);

        char host_str[NI_MAXHOST];
        char port_str[NI_MAXSERV];

        sockaddr_storage local_addr_storage;
        socklen_t local_addr_len = sizeof(local_addr_storage);

        int gs_res = getsockname(socket_fd,
                                 reinterpret_cast<sockaddr *>(&local_addr_storage),
                                 &local_addr_len);

        assert(gs_res == 0);

        int gn_res = getnameinfo(reinterpret_cast<sockaddr *>(&local_addr_storage),
                                 local_addr_len,
                                 host_str, sizeof(host_str),
                                 port_str, sizeof(port_str),
                                 NI_NUMERICHOST | NI_NUMERICSERV);

        if (gn_res == 0)
        {
            std::cout << "UDP Listener started on ";
            if (local_addr_storage.ss_family == AF_INET6)
            {
                // IPv6 addresses use brackets for clarity
                std::cout << "[" << host_str << "]:" << port_str << " (IPv6)" << std::endl;
            }
            else
            {
                std::cout << host_str << ":" << port_str << " (IPv4)" << std::endl;
            }
        }
        else
        {
            std::cerr << "Warning: Could not resolve socket name for printing." << std::endl;
        }
    }

public:
    // selective access
    class server_setup_access_key
    {
        friend std::shared_ptr<i_server> create_server(const char *);

    private:
        server_setup_access_key() {}
    };

    udp(const char *PORT) // like "4004"
    {
        initialize_socket(PORT);
        setup_epoll();
        io_thread = std::jthread([this](std::stop_token token)
                                 { this->event_loop(std::move(token)); });
    }

    ~udp()
    {
        std::cout << "Server closed " << std::endl;

        close(socket_fd);
        close(epoll_fd);
    }

    ssize_t send_packet_to_network(const transport_addr &addr, const char *buf, const size_t &len) override
    {
        return sendto(
            socket_fd,
            buf, len,
            0,
            addr.get_sockaddr(), *addr.get_socklen());
    }

    void set_sesion_control(std::weak_ptr<i_session_control_for_udp> cm, server_setup_access_key)
    {
        session_control_ = cm;
    }
};