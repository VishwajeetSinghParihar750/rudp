#pragma once

#include <netdb.h>
#include <sys/socket.h>
#include <memory>

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

        return is_equal_to(other);
    }
    bool operator!=(const i_sockaddr &other)
    {
        return !(*this == other);
    }

protected:
    virtual bool is_equal_to(const i_sockaddr &other) const = 0;
};