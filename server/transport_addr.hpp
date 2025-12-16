#pragma once

#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <functional>
#include <string>
#include <sstream>

struct transport_addr
{
    sockaddr_storage addr;
    socklen_t addr_len = sizeof(sockaddr_storage);

    const sockaddr *get_sockaddr() const
    {
        return reinterpret_cast<const sockaddr *>(&addr);
    }

    sockaddr *get_mutable_sockaddr()
    {
        return reinterpret_cast<sockaddr *>(&addr);
    }

    const socklen_t *get_socklen() const
    {
        return &addr_len;
    }

    socklen_t *get_mutable_socklen()
    {
        return &addr_len;
    }

    void reset_len()
    {
        addr_len = sizeof(sockaddr_storage);
    }

    bool operator==(const transport_addr &other) const
    {
        if (addr.ss_family != other.addr.ss_family)
            return false;

        if (addr.ss_family == AF_INET)
        {
            auto *a = reinterpret_cast<const sockaddr_in *>(&addr);
            auto *b = reinterpret_cast<const sockaddr_in *>(&other.addr);
            return a->sin_port == b->sin_port &&
                   a->sin_addr.s_addr == b->sin_addr.s_addr;
        }
        else if (addr.ss_family == AF_INET6)
        {
            auto *a = reinterpret_cast<const sockaddr_in6 *>(&addr);
            auto *b = reinterpret_cast<const sockaddr_in6 *>(&other.addr);
            return a->sin6_port == b->sin6_port &&
                   std::memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(in6_addr)) == 0;
        }
        return false;
    }

    std::string to_string() const
    {
        char ipstr[INET6_ADDRSTRLEN];
        int port = 0;
        std::string ip_address;

        if (addr.ss_family == AF_INET)
        {
            const sockaddr_in *s = reinterpret_cast<const sockaddr_in *>(&addr);
            inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
            port = ntohs(s->sin_port);
            ip_address = ipstr;
            return ip_address + ":" + std::to_string(port) + " (IPv4)";
        }
        else if (addr.ss_family == AF_INET6)
        {
            const sockaddr_in6 *s = reinterpret_cast<const sockaddr_in6 *>(&addr);
            inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof(ipstr));
            port = ntohs(s->sin6_port);
            ip_address = ipstr;
            return "[" + ip_address + "]:" + std::to_string(port) + " (IPv6)";
        }
        return "Unknown Address Family (" + std::to_string(addr.ss_family) + ")";
    }
};

struct transport_addr_hasher
{
    std::size_t operator()(const transport_addr &ta) const
    {
        if (ta.addr.ss_family == AF_INET)
        {
            auto *v4 = reinterpret_cast<const sockaddr_in *>(&ta.addr);
            return std::hash<uint32_t>{}(v4->sin_addr.s_addr) ^
                   (std::hash<uint16_t>{}(v4->sin_port) << 1);
        }
        else if (ta.addr.ss_family == AF_INET6)
        {
            auto *v6 = reinterpret_cast<const sockaddr_in6 *>(&ta.addr);
            size_t seed = 0;
            const uint64_t *words = reinterpret_cast<const uint64_t *>(v6->sin6_addr.s6_addr);
            seed ^= std::hash<uint64_t>{}(words[0]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<uint64_t>{}(words[1]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<uint16_t>{}(v6->sin6_port) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
        return 0;
    }
};