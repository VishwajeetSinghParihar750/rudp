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
#include <string.h>
#include <sstream>

#include "i_session_control_for_udp.hpp"
#include "../common/rudp_protocol_packet.hpp"
#include "transport_addr.hpp"
#include "i_udp_for_session_control.hpp"
#include "../common/logger.hpp"

class i_client;

class udp : public i_udp_for_session_control
{
    std::weak_ptr<i_session_control_for_udp> session_control_;

    int socket_fd = -1;
    int epoll_fd = -1;

    std::atomic<bool> server_closed = false;

    std::jthread io_thread;

    void set_non_blocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1)
        {
            logger::getInstance().logCritical("fcntl(F_GETFL) failed.");
            return;
        }
        flags |= O_NONBLOCK;
        if (fcntl(fd, F_SETFL, flags) == -1)
        {
            logger::getInstance().logCritical("fcntl(F_SETFL) failed.");
        }
    }

    void connect_socket(const char *HOST, const char *PORT)
    {
        addrinfo hints{};
        addrinfo *results = nullptr;

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = 0;

        int res = getaddrinfo(HOST, PORT, &hints, &results);
        if (res != 0)
        {
            std::ostringstream oss;
            oss << "getaddrinfo failed: " << gai_strerror(res);
            logger::getInstance().logCritical(oss.str());
            assert(res == 0);
        }

        for (addrinfo *p = results; p != nullptr; p = p->ai_next)
        {
            socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (socket_fd == -1)
            {
                logger::getInstance().logWarning("Failed to create socket. Trying next address.");
                continue;
            }

            set_non_blocking(socket_fd);

            if (::connect(socket_fd, p->ai_addr, p->ai_addrlen) == 0)
            {
                std::ostringstream oss;
                oss << "Successfully connected UDP socket to " << HOST << ":" << PORT;
                logger::getInstance().logInfo(oss.str());
                set_non_blocking(socket_fd);
                break;
            }

            std::ostringstream oss;
            oss << "Failed to connect to " << HOST << ":" << PORT << " with error: " << strerror(errno);
            logger::getInstance().logWarning(oss.str());

            close(socket_fd);
            socket_fd = -1;
        }

        freeaddrinfo(results);

        if (socket_fd == -1)
        {
            logger::getInstance().logCritical("Failed to connect UDP socket after trying all addresses.");
        }
        assert(socket_fd != -1);
    }

    void setup_epoll()
    {
        epoll_event event;

        epoll_fd = epoll_create1(0);

        if (epoll_fd == -1)
        {
            std::ostringstream oss;
            oss << "epoll_create1 failed: " << strerror(errno);
            logger::getInstance().logCritical(oss.str());
        }
        assert(epoll_fd != -1);

        event.data.fd = socket_fd;
        event.events = EPOLLIN;

        int epoll_ctl_rv = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event);

        if (epoll_ctl_rv == -1)
        {
            std::ostringstream oss;
            oss << "epoll_ctl failed: " << strerror(errno);
            logger::getInstance().logCritical(oss.str());
        }
        assert(epoll_ctl_rv == 0);
        logger::getInstance().logInfo("Epoll setup successful for UDP socket.");
    }

    void event_loop(std::stop_token token)
    {

        constexpr int MAX_EVENTS = 2048;
        epoll_event event, events[MAX_EVENTS];
        std::shared_ptr<i_session_control_for_udp> session_control_sp;

        int event_cnt = 0;
        logger::getInstance().logInfo("UDP I/O thread started.");
        while (!token.stop_requested() && !server_closed.load())
        {
            event_cnt = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);

            if (token.stop_requested())
                break;

            if (event_cnt == -1)
            {
                if (errno != EINTR)
                {
                    std::ostringstream oss;
                    oss << "server epoll_wait error: " << strerror(errno);
                    logger::getInstance().logCritical(oss.str());
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

                    if (ioctl(socket_fd, FIONREAD, &bytes_avail) == -1)
                    {
                        std::ostringstream oss;
                        oss << "ioctl FIONREAD failed: " << strerror(errno);
                        logger::getInstance().logError(oss.str());
                        continue;
                    }

                    if (bytes_avail == 0)
                    {

                        logger::getInstance().logWarning("EPOLLIN triggered but FIONREAD returned 0 bytes.");
                        server_closed.store(true);
                        auto sp = session_control_.lock();
                        if (sp)
                            sp->on_server_disconnected();
                        break;
                    }

                    std::unique_ptr<rudp_protocol_packet> pkt = std::make_unique<rudp_protocol_packet>(bytes_avail);

                    int n = recv(socket_fd, pkt->get_buffer(), pkt->get_capacity(), 0);

                    if (n > 0)
                    {
                        pkt->set_length(n);
                        session_control_sp = session_control_.lock();
                        if (session_control_sp != nullptr)
                        {
                            std::ostringstream oss;
                            oss << "Received " << n << " bytes from network. Forwarding to session control.";
                            logger::getInstance().logInfo(oss.str());
                            session_control_sp->on_transport_receive(std::move(pkt));
                        }
                        else
                        {
                            logger::getInstance().logWarning("Received data, but session control is expired. Stopping I/O thread.");
                            return;
                        }
                    }
                    else if (n == 0)
                    {
                        logger::getInstance().logWarning("recv returned 0 (should not happen for connected UDP).");
                    }
                    else // n < 0
                    {
                        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
                        {
                            std::ostringstream oss;
                            oss << "recv error: " << strerror(errno);
                            logger::getInstance().logError(oss.str());
                        }
                    }
                }
                else
                {
                    std::ostringstream oss;
                    oss << "Epoll returned event for unknown fd: " << current_fd;
                    logger::getInstance().logWarning(oss.str());
                }
            }
        }
        logger::getInstance().logInfo("UDP I/O thread stopped.");
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
        logger::getInstance().logInfo("Creating UDP object and connecting socket.");
        connect_socket(host, port);
        setup_epoll();
        io_thread = std::jthread([this](std::stop_token token)
                                 { this->event_loop(std::move(token)); });
    }

    ~udp()
    {
        io_thread.request_stop();
        if (io_thread.joinable())
            io_thread.join();

        std::cout << "connection closed " << std::endl;
        logger::getInstance().logInfo("UDP object destruction: closing socket and epoll fd.");

        if (socket_fd != -1)
            close(socket_fd);
        if (epoll_fd != -1)
            close(epoll_fd);
    }

    ssize_t send_packet_to_network(const char *buf, const size_t &len) override
    {
        ssize_t sent_bytes = send(
            socket_fd,
            buf, len,
            0);

        if (sent_bytes == -1)
        {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
            {
                auto sp = session_control_.lock();

                server_closed.store(true);
                if (sp)
                    sp->on_server_disconnected();

                std::ostringstream oss;
                oss << "send failed: " << strerror(errno);
                logger::getInstance().logError(oss.str());
            }
            else
            {
                logger::getInstance().logWarning("send blocked (EAGAIN/EWOULDBLOCK).");
            }
        }
        else
        {
            std::ostringstream oss;
            oss << "Sent " << sent_bytes << " bytes to network.";
            logger::getInstance().logInfo(oss.str());
        }

        return sent_bytes;
    }

    void set_sesion_control(std::weak_ptr<i_session_control_for_udp> cm, client_setup_access_key)
    {
        session_control_ = cm;
        logger::getInstance().logInfo("Session control weak pointer set in UDP.");
    }
};