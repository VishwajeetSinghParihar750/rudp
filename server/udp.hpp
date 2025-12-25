#pragma once

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cassert>
#include <thread>
#include <memory>

#include "../common/rudp_protocol_packet.hpp"
#include "../common/logger.hpp"
#include "../common/transport_addr.hpp"
#include "../common/timer_manager.hpp"
#include "../common/i_timer_service.hpp"

#include "i_udp_for_session_control.hpp"
#include "i_session_control_for_udp.hpp"

class i_server;

class udp : public i_udp_for_session_control, public i_timer_service
{
    std::weak_ptr<i_session_control_for_udp> session_control_;
    std::shared_ptr<timer_manager> timers_;

    int socket_fd = -1;
    int epoll_fd = -1;
    int wake_fd = -1;

    std::jthread io_thread;

    static void set_non_blocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1)
        {
            LOG_CRITICAL("[set_non_blocking] fcntl(F_GETFL) failed: "
                         << strerror(errno));
            assert(false);
        }

        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            LOG_CRITICAL("[set_non_blocking] fcntl(F_SETFL) failed: "
                         << strerror(errno));
            assert(false);
        }

        LOG_INFO("[set_non_blocking] FD " << fd << " set to non-blocking.");
    }

    void setup_socket(const char *PORT)
    {
        LOG_INFO("[setup_socket] Initializing UDP socket on port " << PORT);

        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_PASSIVE;

        int rc = getaddrinfo(nullptr, PORT, &hints, &res);
        if (rc != 0)
        {
            LOG_CRITICAL("[setup_socket] getaddrinfo failed: "
                         << gai_strerror(rc));
            assert(false);
        }

        for (addrinfo *p = res; p; p = p->ai_next)
        {
            socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (socket_fd < 0)
            {
                LOG_WARN("[setup_socket] socket() failed: "
                         << strerror(errno));
                continue;
            }

            set_non_blocking(socket_fd);

            int one = 1;
            if (setsockopt(socket_fd,
                           SOL_SOCKET,
                           SO_REUSEADDR,
                           &one,
                           sizeof(one)) == -1)
            {
                LOG_ERROR("[setup_socket] setsockopt(SO_REUSEADDR) failed: "
                          << strerror(errno));
            }

            if (bind(socket_fd, p->ai_addr, p->ai_addrlen) == 0)
            {
                LOG_INFO("[setup_socket] Successfully bound UDP socket.");
                break;
            }

            LOG_WARN("[setup_socket] bind() failed: "
                     << strerror(errno));

            close(socket_fd);
            socket_fd = -1;
        }

        freeaddrinfo(res);

        if (socket_fd == -1)
        {
            LOG_CRITICAL("[setup_socket] Failed to bind UDP socket.");
            assert(false);
        }
    }

    void setup_epoll()
    {
        LOG_INFO("[setup_epoll] Setting up epoll.");

        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1)
        {
            LOG_CRITICAL("[setup_epoll] epoll_create1 failed: "
                         << strerror(errno));
            assert(false);
        }

        wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd == -1)
        {
            LOG_CRITICAL("[setup_epoll] eventfd failed: "
                         << strerror(errno));
            assert(false);
        }

        epoll_event ev{};
        ev.events = EPOLLIN;

        ev.data.fd = socket_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &ev) == -1)
        {
            LOG_CRITICAL("[setup_epoll] epoll_ctl add socket_fd failed: "
                         << strerror(errno));
            assert(false);
        }

        ev.data.fd = wake_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &ev) == -1)
        {
            LOG_CRITICAL("[setup_epoll] epoll_ctl add wake_fd failed: "
                         << strerror(errno));
            assert(false);
        }

        LOG_INFO("[setup_epoll] Epoll setup complete.");
    }

    void event_loop(std::stop_token st)
    {
        LOG_INFO("[event_loop] UDP server I/O thread started.");

        constexpr int MAX_EVENTS = 1024;
        epoll_event events[MAX_EVENTS];

        while (!st.stop_requested())
        {
            duration_ms timeout =
                timers_->next_timeout(duration_ms(5000));

            int n = epoll_wait(epoll_fd,
                               events,
                               MAX_EVENTS,
                               timeout.count());

            if (n == -1)
            {
                if (errno != EINTR)
                {
                    LOG_ERROR("[event_loop] epoll_wait failed: "
                              << strerror(errno));
                }
                continue;
            }

            for (int i = 0; i < n; ++i)
            {
                int fd = events[i].data.fd;

                if (fd == socket_fd)
                {
                    handle_udp();
                }
                else if (fd == wake_fd)
                {
                    uint64_t v;
                    int _ = read(wake_fd, &v, sizeof(v)); // drain
                }
            }

            // Always process timers after wakeup
            timers_->process_expired();
        }

        LOG_INFO("[event_loop] UDP server I/O thread stopping.");
    }

    void handle_udp()
    {
        size_t avail = 0;
        if (ioctl(socket_fd, FIONREAD, &avail) == -1)
        {
            LOG_ERROR("[handle_udp] ioctl(FIONREAD) failed: "
                      << strerror(errno));
            return;
        }

        if (avail == 0)
        {
            LOG_WARN("[handle_udp] EPOLLIN but no data available.");
            return;
        }

        auto pkt = std::make_unique<rudp_protocol_packet>(avail);
        auto addr = std::make_unique<transport_addr>();

        ssize_t n = recvfrom(socket_fd,
                             pkt->get_buffer(),
                             pkt->get_capacity(),
                             0,
                             addr->get_mutable_sockaddr(),
                             addr->get_mutable_socklen());

        if (n <= 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_ERROR("[handle_udp] recvfrom failed: "
                          << strerror(errno));
            }
            return;
        }

        pkt->set_length(n);

        LOG_TEST("[handle_udp] Received "
                 << n << " bytes from "
                 << addr->to_string());

        if (auto sc = session_control_.lock())
        {
            sc->on_transport_receive(std::move(pkt),
                                     std::move(addr));
        }
        else
        {
            LOG_WARN("[handle_udp] Session control expired.");
        }
    }

public:
    class server_setup_access_key
    {
        friend std::shared_ptr<i_server> create_server(const char *);
        server_setup_access_key() {}
    };

    udp(const char *PORT,
        std::shared_ptr<timer_manager> tm)
        : timers_(std::move(tm))
    {
        assert(timers_);

        setup_socket(PORT);
        setup_epoll();

        io_thread = std::jthread(
            [this](std::stop_token st)
            { event_loop(st); });
    }

    udp(const udp &) = delete;
    udp &operator=(const udp &) = delete;
    udp(udp &&) = delete;
    udp &operator=(udp &&) = delete;

    ~udp()
    {
        LOG_INFO("UDP server destructor: closing resources.");

        if (socket_fd != -1)
            close(socket_fd);
        if (wake_fd != -1)
            close(wake_fd);
        if (epoll_fd != -1)
            close(epoll_fd);
    }

    // i_timer_service
    void add_timer(timer_info_ptr t) override
    {
        bool earlier = timers_->add_timer(std::move(t));
        if (earlier)
        {
            uint64_t one = 1;
            int _ = write(wake_fd, &one, sizeof(one));
        }
    }

    ssize_t send_packet_to_network(const transport_addr &addr,
                                   const char *buf,
                                   const size_t &len) override
    {
        ssize_t sent = sendto(socket_fd,
                              buf,
                              len,
                              0,
                              addr.get_sockaddr(),
                              *addr.get_socklen());

        if (sent == -1)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_ERROR("[send_packet] sendto failed to "
                          << addr.to_string()
                          << ": " << strerror(errno));
            }
        }
        else
        {
            LOG_TEST("[send_packet] Sent "
                     << sent << " bytes to "
                     << addr.to_string());
        }

        return sent;
    }

    void set_sesion_control(std::weak_ptr<i_session_control_for_udp> sc,
                            server_setup_access_key)
    {
        session_control_ = sc;
        LOG_INFO("Session control weak pointer set in UDP server.");
    }
};
