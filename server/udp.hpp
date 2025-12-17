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
#include <cerrno>
#include <cstring>

#include "../common/rudp_protocol_packet.hpp"
#include "../common/logger.hpp"
#include "../common/transport_addr.hpp"

#include "i_udp_for_session_control.hpp"
#include "i_session_control_for_udp.hpp"

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
        if (flags == -1)
        {
            logger::getInstance().logError("[set_non_blocking] fcntl(F_GETFL) failed: " + std::string(strerror(errno)));
            assert(false);
        }
        flags |= O_NONBLOCK;
        if (fcntl(fd, F_SETFL, flags) == -1)
        {
            logger::getInstance().logError("[set_non_blocking] fcntl(F_SETFL, O_NONBLOCK) failed: " + std::string(strerror(errno)));
            assert(false);
        }
        logger::getInstance().logInfo("[set_non_blocking] Socket " + std::to_string(fd) + " set to non-blocking.");
    }

    void initialize_socket(const char *PORT)
    {
        logger::getInstance().logInfo("[initialize_socket] Initializing UDP socket on port " + std::string(PORT));
        addrinfo hints{};
        addrinfo *results = nullptr;

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;

        int res = getaddrinfo(NULL, PORT, &hints, &results);
        if (res != 0)
        {
            logger::getInstance().logCritical("[initialize_socket] getaddrinfo failed: " + std::string(gai_strerror(res)));
            assert(false);
        }

        for (addrinfo *p = results; p != nullptr; p = p->ai_next)
        {
            socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (socket_fd == -1)
            {
                logger::getInstance().logWarning("[initialize_socket] Failed to create socket: " + std::string(strerror(errno)));
                continue;
            }

            set_non_blocking(socket_fd);

            int optval = 1;
            int sopres = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
            if (sopres == -1)
            {
                logger::getInstance().logError("[initialize_socket] setsockopt(SO_REUSEADDR) failed: " + std::string(strerror(errno)));
                close(socket_fd);
                socket_fd = -1;
                continue;
            }

            int res_ = bind(socket_fd, p->ai_addr, p->ai_addrlen);

            if (res_ == 0)
            {
                logger::getInstance().logInfo("[initialize_socket] Successfully bound socket.");
                break;
            }

            logger::getInstance().logWarning("[initialize_socket] Failed to bind socket: " + std::string(strerror(errno)));
            close(socket_fd);
            socket_fd = -1;
        }

        freeaddrinfo(results);

        if (socket_fd == -1)
        {
            logger::getInstance().logCritical("[initialize_socket] Failed to initialize UDP socket.");
        }
        assert(socket_fd != -1);
    }

    void setup_epoll()
    {
        logger::getInstance().logInfo("[setup_epoll] Setting up epoll.");
        epoll_event event;

        epoll_fd = epoll_create1(0);

        if (epoll_fd == -1)
        {
            logger::getInstance().logCritical("[setup_epoll] epoll_create1 failed: " + std::string(strerror(errno)));
        }
        assert(epoll_fd != -1);

        event.data.fd = socket_fd;
        event.events = EPOLLIN;

        int epoll_ctl_rv = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event);

        if (epoll_ctl_rv == -1)
        {
            logger::getInstance().logCritical("[setup_epoll] epoll_ctl failed to add socket_fd: " + std::string(strerror(errno)));
        }
        assert(epoll_ctl_rv == 0);
        logger::getInstance().logInfo("[setup_epoll] UDP socket added to epoll instance.");
    }

    void event_loop(std::stop_token token)
    {
        logger::getInstance().logInfo("[event_loop] UDP I/O thread started.");
        print_listener_info();

        constexpr int MAX_EVENTS = 2048;
        epoll_event events[MAX_EVENTS];
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
                    logger::getInstance().logError("[event_loop] epoll_wait error: " + std::string(strerror(errno)));
                }
                continue;
            }

            for (auto ind = 0; ind < event_cnt; ind++)
            {
                int current_fd = events[ind].data.fd;
                if (current_fd == socket_fd && (events[ind].events & EPOLLIN))
                {
                    size_t bytes_avail = 0;

                    if (ioctl(socket_fd, FIONREAD, &bytes_avail) == -1)
                    {
                        logger::getInstance().logError("[event_loop] ioctl(FIONREAD) failed: " + std::string(strerror(errno)));
                        continue;
                    }

                    if (bytes_avail == 0)
                    {
                        logger::getInstance().logWarning("[event_loop] Received EPOLLIN but FIONREAD reported 0 bytes available.");
                        continue;
                    }

                    std::unique_ptr<rudp_protocol_packet> pkt = std::make_unique<rudp_protocol_packet>(bytes_avail);
                    std::unique_ptr<transport_addr> addr = std::make_unique<transport_addr>();

                    int n = recvfrom(socket_fd, pkt->get_buffer(), pkt->get_capacity(), 0, addr->get_mutable_sockaddr(), addr->get_mutable_socklen());

                    if (n > 0)
                    {
                        pkt->set_length(n);
                        session_control_sp = session_control_.lock();
                        if (session_control_sp != nullptr)
                        {
                            logger::getInstance().logTest("[event_loop] Received " + std::to_string(n) + " bytes from " + addr->to_string() + ". Forwarding to Session Control.");
                            session_control_sp->on_transport_receive(std::move(pkt), std::move(addr));
                        }
                        else
                        {
                            logger::getInstance().logWarning("[event_loop] Received packet, but Session Control weak pointer expired.");
                        }
                    }
                    else if (n == -1)
                    {
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            logger::getInstance().logError("[event_loop] recvfrom failed: " + std::string(strerror(errno)));
                        }
                        else
                        {
                            logger::getInstance().logTest("[event_loop] recvfrom returned EAGAIN/EWOULDBLOCK (expected for non-blocking).");
                        }
                    }
                }
                else
                {
                    logger::getInstance().logError("[event_loop] Epoll event on unexpected fd/event: fd=" + std::to_string(current_fd) + ", events=" + std::to_string(events[ind].events));
                }
            }
        }
        logger::getInstance().logInfo("[event_loop] UDP I/O thread stopping.");
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

        if (gs_res != 0)
        {
            logger::getInstance().logError("[print_listener_info] getsockname failed: " + std::string(strerror(errno)));
        }
        assert(gs_res == 0);

        int gn_res = getnameinfo(reinterpret_cast<sockaddr *>(&local_addr_storage),
                                 local_addr_len,
                                 host_str, sizeof(host_str),
                                 port_str, sizeof(port_str),
                                 NI_NUMERICHOST | NI_NUMERICSERV);

        if (gn_res == 0)
        {
            std::string log_msg = "UDP Listener started on ";
            if (local_addr_storage.ss_family == AF_INET6)
            {
                log_msg += "[" + std::string(host_str) + "]:" + std::string(port_str) + " (IPv6)";
            }
            else
            {
                log_msg += std::string(host_str) + ":" + std::string(port_str) + " (IPv4)";
            }
            logger::getInstance().logCritical(log_msg);
        }
        else
        {
            logger::getInstance().logWarning("Could not resolve socket name for printing: " + std::string(gai_strerror(gn_res)));
        }
    }

public:
    class server_setup_access_key
    {
        friend std::shared_ptr<i_server> create_server(const char *);

    private:
        server_setup_access_key() {}
    };

    udp(const char *PORT)
    {
        initialize_socket(PORT);
        setup_epoll();
        io_thread = std::jthread([this](std::stop_token token)
                                 { this->event_loop(std::move(token)); });
    }

    ~udp()
    {
        logger::getInstance().logCritical("UDP destructor called. Closing socket and epoll fd.");

        if (socket_fd != -1)
        {
            close(socket_fd);
            logger::getInstance().logInfo("Socket " + std::to_string(socket_fd) + " closed.");
        }
        if (epoll_fd != -1)
        {
            close(epoll_fd);
            logger::getInstance().logInfo("Epoll FD " + std::to_string(epoll_fd) + " closed.");
        }
    }

    ssize_t send_packet_to_network(const transport_addr &addr, const char *buf, const size_t &len) override
    {
        ssize_t sent_bytes = sendto(
            socket_fd,
            buf, len,
            0,
            addr.get_sockaddr(), *addr.get_socklen());

        if (sent_bytes == -1)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                logger::getInstance().logError("sendto failed to " + addr.to_string() + ": " + std::string(strerror(errno)));
            }
        }
        else
        {
            logger::getInstance().logTest("Sent " + std::to_string(sent_bytes) + " bytes to " + addr.to_string());
        }

        return sent_bytes;
    }

    void set_sesion_control(std::weak_ptr<i_session_control_for_udp> cm, server_setup_access_key)
    {
        session_control_ = cm;
        logger::getInstance().logInfo("Session Control weak pointer set.");
    }
};