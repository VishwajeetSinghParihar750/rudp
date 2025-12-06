#pragma once

#include <netdb.h>
#include <sys/socket.h>
#include <memory>
#include <memory.h>

class i_sockaddr;
inline bool compare_sockaddrs_content(const i_sockaddr &a, const i_sockaddr &b);

class i_sockaddr
{

public:
    virtual ~i_sockaddr() = default;

    virtual const sockaddr *get_sockaddr() const = 0;
    virtual socklen_t get_socklen() const = 0;

    virtual sockaddr *get_mutable_sockaddr() = 0;
    virtual socklen_t *get_mutable_socklen() = 0;

    bool operator==(const i_sockaddr &other)
    {
        if (this == &other)
            return true;

        return compare_sockaddrs_content(*this, other);
    }
    bool operator!=(const i_sockaddr &other)
    {
        return !(*this == other);
    }


    virtual void set_socklen(socklen_t len) = 0;
    
    virtual std::unique_ptr<i_sockaddr> clone() const = 0;

protected:
};

inline bool compare_sockaddrs_content(const i_sockaddr &a, const i_sockaddr &b)
{
    const sockaddr *sa = a.get_sockaddr();
    socklen_t len_a = a.get_socklen();

    const sockaddr *sb = b.get_sockaddr();
    socklen_t len_b = b.get_socklen();

    if (sa->sa_family != sb->sa_family)
    {
        return false;
    }

    if (len_a != len_b)
    {
        return false;
    }

    switch (sa->sa_family)
    {
    case AF_INET:
    {
        const sockaddr_in *sa_in_a = reinterpret_cast<const sockaddr_in *>(sa);
        const sockaddr_in *sa_in_b = reinterpret_cast<const sockaddr_in *>(sb);

        return (sa_in_a->sin_port == sa_in_b->sin_port) &&
               (sa_in_a->sin_addr.s_addr == sa_in_b->sin_addr.s_addr);
    }

    case AF_INET6:
    {
        const sockaddr_in6 *sa_in6_a = reinterpret_cast<const sockaddr_in6 *>(sa);
        const sockaddr_in6 *sa_in6_b = reinterpret_cast<const sockaddr_in6 *>(sb);

        return (sa_in6_a->sin6_port == sa_in6_b->sin6_port) &&
               (memcmp(&sa_in6_a->sin6_addr,
                       &sa_in6_b->sin6_addr,
                       sizeof(in6_addr)) == 0);
    }
    default:
        return false;
    }
}