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
#include <atomic>

#include "../common/rudp_protocol_packet.hpp"
#include "../common/logger.hpp"
#include "../common/timer_manager.hpp"
#include "../common/i_timer_service.hpp"

#include "i_udp_for_session_control.hpp"
#include "i_session_control_for_udp.hpp"

class i_client;

class udp
    : public i_udp_for_session_control,
      public i_timer_service
{
    std::weak_ptr<i_session_control_for_udp> session_control_;
    std::shared_ptr<timer_manager> timers_;

    int socket_fd = -1;
    int epoll_fd = -1;
    int wake_fd = -1;

    std::atomic<bool> server_closed{false};
    std::jthread io_thread;

    void set_non_blocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1)
        {
            LOG_CRITICAL("[set_non_blocking] fcntl(F_GETFL) failed.");
            return;
        }
        flags |= O_NONBLOCK;
        if (fcntl(fd, F_SETFL, flags) == -1)
        {
            LOG_CRITICAL("[set_non_blocking] fcntl(F_SETFL) failed.");
        }
    }

    void connect_socket(const char *HOST, const char *PORT)
    {
        addrinfo hints{}, *results = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        int res = getaddrinfo(HOST, PORT, &hints, &results);
        if (res != 0)
        {
            LOG_CRITICAL("[connect_socket] getaddrinfo failed: "
                         << gai_strerror(res));
            assert(false);
        }

        for (addrinfo *p = results; p; p = p->ai_next)
        {
            socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (socket_fd == -1)
            {
                LOG_WARN("[connect_socket] Failed to create socket.");
                continue;
            }

            set_non_blocking(socket_fd);

            if (::connect(socket_fd, p->ai_addr, p->ai_addrlen) == 0)
            {
                LOG_INFO("[connect_socket] Successfully connected UDP socket to "
                         << HOST << ":" << PORT);
                break;
            }

            LOG_WARN("[connect_socket] Failed to connect: "
                     << strerror(errno));
            close(socket_fd);
            socket_fd = -1;
        }

        freeaddrinfo(results);

        if (socket_fd == -1)
        {
            LOG_CRITICAL("[connect_socket] Failed to connect UDP socket.");
        }
        assert(socket_fd != -1);
    }

    void setup_epoll()
    {
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1)
        {
            LOG_CRITICAL("[setup_epoll] epoll_create1 failed: "
                         << strerror(errno));
        }
        assert(epoll_fd != -1);

        wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd == -1)
        {
            LOG_CRITICAL("[setup_epoll] eventfd failed: "
                         << strerror(errno));
        }
        assert(wake_fd != -1);

        epoll_event ev{};
        ev.events = EPOLLIN;

        ev.data.fd = socket_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &ev) == -1)
        {
            LOG_CRITICAL("[setup_epoll] epoll_ctl add socket failed: "
                         << strerror(errno));
        }

        ev.data.fd = wake_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &ev) == -1)
        {
            LOG_CRITICAL("[setup_epoll] epoll_ctl add wake_fd failed: "
                         << strerror(errno));
        }

        LOG_INFO("[setup_epoll] Epoll setup successful for client UDP.");
    }

    void event_loop(std::stop_token token)
    {
        constexpr int MAX_EVENTS = 2048;
        epoll_event events[MAX_EVENTS];

        LOG_INFO("[event_loop] Client UDP I/O thread started.");

        while (!token.stop_requested() && !server_closed.load())
        {
            duration_ms timeout =
                timers_->next_timeout(duration_ms(5000));

            int event_cnt = epoll_wait(epoll_fd,
                                       events,
                                       MAX_EVENTS,
                                       timeout.count());

            if (event_cnt == -1)
            {
                if (errno != EINTR)
                {
                    LOG_ERROR("[event_loop] epoll_wait error: "
                              << strerror(errno));
                }
                continue;
            }

            for (int i = 0; i < event_cnt; i++)
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

            timers_->process_expired();
        }

        LOG_INFO("[event_loop] Client UDP I/O thread stopped.");
    }

    void handle_udp()
    {
        size_t bytes_avail = 0;
        if (ioctl(socket_fd, FIONREAD, &bytes_avail) == -1)
        {
            LOG_ERROR("[handle_udp] ioctl(FIONREAD) failed: "
                      << strerror(errno));
            return;
        }

        if (bytes_avail == 0)
        {
            LOG_WARN("[handle_udp] Server closed connection.");
            server_closed.store(true);
            if (auto sp = session_control_.lock())
                sp->on_server_disconnected();
            return;
        }

        auto pkt =
            std::make_unique<rudp_protocol_packet>(bytes_avail);

        ssize_t n = recv(socket_fd,
                         pkt->get_buffer(),
                         pkt->get_capacity(),
                         0);

        if (n > 0)
        {
            pkt->set_length(n);
            if (auto sp = session_control_.lock())
            {
                LOG_INFO("[handle_udp] Received " << n << " bytes.");
                sp->on_transport_receive(std::move(pkt));
            }
        }
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_ERROR("[handle_udp] recv failed: "
                      << strerror(errno));
        }
    }

public:
    class client_setup_access_key
    {
        friend std::shared_ptr<i_client>
        create_client(const char *, const char *);
        client_setup_access_key() {}
    };

    udp(const char *host,
        const char *port,
        std::shared_ptr<timer_manager> tm)
        : timers_(std::move(tm))
    {
        assert(timers_);
        LOG_INFO("Creating client UDP and connecting socket.");
        connect_socket(host, port);
        setup_epoll();

        io_thread = std::jthread(
            [this](std::stop_token token)
            { event_loop(token); });
    }

    udp(const udp &) = delete;
    udp &operator=(const udp &) = delete;
    udp(udp &&) = delete;
    udp &operator=(udp &&) = delete;

    ~udp()
    {
        io_thread.request_stop();
        if (io_thread.joinable())
            io_thread.join();

        LOG_INFO("Client UDP destructor: closing resources.");

        if (socket_fd != -1)
            close(socket_fd);
        if (wake_fd != -1)
            close(wake_fd);
        if (epoll_fd != -1)
            close(epoll_fd);
    }

    // i_timer_service
    void add_timer(std::shared_ptr<timer_info> t) override
    {
        bool earlier = timers_->add_timer(std::move(t));
        if (earlier)
        {
            uint64_t one = 1;
            int _ = write(wake_fd, &one, sizeof(one));
        }
    }

    ssize_t send_packet_to_network(const char *buf,
                                   const size_t &len) override
    {
        ssize_t sent = send(socket_fd, buf, len, 0);

        if (sent == -1)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_ERROR("[send] send failed: " << "len " << len
                                                 << strerror(errno));
                server_closed.store(true);
                if (auto sp = session_control_.lock())
                    sp->on_server_disconnected();
            }
            else
            {
                LOG_WARN("[send] send would block.");
            }
        }
        else
        {
            LOG_INFO("[send] Sent " << sent << " bytes.");
        }

        return sent;
    }

    void set_sesion_control(std::weak_ptr<i_session_control_for_udp> sc,
                            client_setup_access_key)
    {
        session_control_ = sc;
        LOG_INFO("Session control weak pointer set in client UDP.");
    }
};
