#pragma once

#include <cstring>
#include "i_sockaddr.hpp"

class wrapper_sockaddr : public i_sockaddr
{
private:
    sockaddr_storage addr_storage;
    socklen_t addr_len;

public:
    wrapper_sockaddr()
    {
        addr_len = sizeof(addr_storage);
        std::memset(&addr_storage, 0, sizeof(addr_storage));
    }

    wrapper_sockaddr(const sockaddr *addr, socklen_t len)
    {
        if (len > sizeof(addr_storage))
        {
            throw std::runtime_error("sockaddr length exceeds storage capacity.");
        }
        addr_len = len;
        std::memcpy(&addr_storage, addr, len);
    }

    const sockaddr *get_sockaddr() const override
    {
        return reinterpret_cast<const sockaddr *>(&addr_storage);
    }

    socklen_t get_socklen() const override
    {
        return addr_len;
    }

    sockaddr *get_mutable_sockaddr() override
    {
        return reinterpret_cast<sockaddr *>(&addr_storage);
    }

    socklen_t *get_mutable_socklen() override
    {
        return &addr_len;
    }

    void set_socklen(socklen_t len) override
    {
        addr_len = len;
    }

    std::unique_ptr<i_sockaddr> clone() const override
    {
        return std::make_unique<wrapper_sockaddr>(
            get_sockaddr(),
            get_socklen());
    }
};