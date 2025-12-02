
#pragma once

#include <inttypes.h>
#include <memory>

#include "types.hpp"
#include "channel_manager.hpp"

class channel_type
{
};

class rudp_server
{

    std::shared_ptr<channel_manager> channel_mananger_;

public:
    auto add_channel(channel_type type)
    {
        return channel_mananger_->add_channel(type);
    }

    bool read_poll(char *buffer, size_t len)
    {
    }
    // wait for something to come
    void read_wait(client_id &client_id_, channel_id &channel_id, char *buffer, size_t len)
    {
        //
    }

    ssize_t write(const client_id &client_id_, const channel_id &channel_id_, const char *buf, size_t length)
    {
    }
};