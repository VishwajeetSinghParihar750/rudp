#pragma once

#include <algorithm>
#include <cstddef>

#include "i_packet.hpp"

class rudp_protocol_packet : public i_packet
{
    size_t length;
    size_t capacity;
    char *mem;

public:
    rudp_protocol_packet() = default;

    rudp_protocol_packet(size_t n)
        : length(0), capacity(n), mem(new char[n]) {}

    rudp_protocol_packet(size_t n, const char *buf)
        : length(n), capacity(n), mem(new char[n])
    {
        std::copy(buf, buf + n, mem);
    }

    void resize_packet(size_t new_capacity, bool copy_data = true)
    {
        if (new_capacity == capacity)
            return;

        char *new_mem = nullptr;
        if (new_capacity > 0)
        {
            new_mem = new char[new_capacity];

            if (copy_data && capacity > 0)
            {
                size_t bytes_to_copy = std::min(length, new_capacity);
                std::copy(mem, mem + bytes_to_copy, new_mem);
                length = bytes_to_copy;
            }
            else
            {
                length = 0;
            }
        }
        else
        {
            length = 0;
        }

        if (capacity > 0)
            delete[] mem;

        mem = new_mem;
        capacity = new_capacity;
    }

    char *get_buffer() override { return mem; }
    char *get_const_buffer() const override { return mem; }
    size_t get_capacity() const override { return capacity; }
    size_t get_length() const override { return length; }
    void set_length(size_t newlen) override { length = newlen; }

    ~rudp_protocol_packet() override
    {
        if (capacity > 0)
            delete[] mem;
    }

    rudp_protocol_packet(const rudp_protocol_packet &other)
        : i_packet(other), length(other.length), capacity(other.capacity),
          mem(new char[other.capacity])
    {
        std::copy(other.mem, other.mem + other.capacity, mem);
    }

    rudp_protocol_packet &operator=(const rudp_protocol_packet &other)
    {
        rudp_protocol_packet temp(other);
        *this = std::move(temp);
        return *this;
    }

    rudp_protocol_packet(rudp_protocol_packet &&other) noexcept
        : i_packet(std::move(other)), length(other.length), capacity(other.capacity),
          mem(other.mem)
    {
        other.mem = nullptr;
        other.length = 0;
        other.capacity = 0;
    }

    rudp_protocol_packet &operator=(rudp_protocol_packet &&other) noexcept
    {
        if (this != &other)
        {
            delete[] mem;

            mem = other.mem;
            length = other.length;
            capacity = other.capacity;

            other.mem = nullptr;
            other.length = 0;
            other.capacity = 0;
        }
        return *this;
    }
};