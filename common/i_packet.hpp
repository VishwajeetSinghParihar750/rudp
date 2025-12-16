#pragma once

#include <sys/types.h>

class i_packet
{

public:
    virtual ~i_packet() = default;

    virtual char *get_buffer() = 0;
    virtual char *get_const_buffer() const = 0;
    virtual size_t get_capacity() const = 0;

    virtual size_t get_length() const = 0;
    virtual void set_length(size_t) = 0;
};