
#pragma once

#include <inttypes.h>
#include <memory>
#include "../common/types.hpp"

class i_client
{

public:
    virtual ~i_client() = default;

    virtual void add_channel(channel_id, channel_type) = 0;

    virtual ssize_t read_from_channel_nonblocking(channel_id &channel_id_, char *buf, const size_t len) = 0;
    // wait for something to come
    virtual ssize_t read_from_channel_blocking(channel_id &channel_id_, char *buf, const size_t len) = 0;

    virtual ssize_t write_to_channel(const channel_id &channel_id_, const char *buf, const size_t len) = 0;
};