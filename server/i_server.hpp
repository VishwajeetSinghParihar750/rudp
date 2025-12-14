
#pragma once

#include <inttypes.h>
#include <memory>
#include "types.hpp"

class i_server
{

public:
    virtual void add_channel(channel_id, channel_type type) = 0;

    virtual ssize_t read_from_channel_nonblocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) = 0;
    // wait for something to come
    virtual ssize_t read_from_channel_blocking(channel_id &channel_id_, client_id &client_id_, char *buf, const size_t len) = 0;

    virtual ssize_t write_to_channel(const channel_id &channel_id_, const client_id &client_id_, const char *buf, const size_t len) = 0;

    virtual ~i_server() = default;
};