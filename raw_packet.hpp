#pragma once

#include <algorithm>
#include <cstddef>

#include "i_packet.hpp"

class raw_packet : public i_packet
{
    size_t length;
    size_t capacity;
    char *mem;

public:
    raw_packet() = default;

    raw_packet(size_t n)
        : length(0), capacity(n), mem(new char[n]) {}

    raw_packet(size_t n, const char *buf)
        : length(n), capacity(n), mem(new char[n])
    {
        std::copy(buf, buf + n, mem);
    }

    char *get_buffer() override { return mem; }
    char *get_const_buffer() const override { return mem; }
    size_t get_capacity() const override { return capacity; }
    size_t get_length() const override { return length; }
    void set_length(size_t newlen) override { length = newlen; }

    ~raw_packet() override
    {
        if (capacity > 0)
            delete[] mem;
    }

    raw_packet(const raw_packet &other)
        : i_packet(other), length(other.length), capacity(other.capacity),
          mem(new char[other.capacity])
    {
        std::copy(other.mem, other.mem + other.capacity, mem);
    }

    raw_packet &operator=(const raw_packet &other)
    {
        raw_packet temp(other);
        *this = std::move(temp);
        return *this;
    }

    raw_packet(raw_packet &&other) noexcept
        : i_packet(std::move(other)), length(other.length), capacity(other.capacity),
          mem(other.mem)
    {
        other.mem = nullptr;
        other.length = 0;
        other.capacity = 0;
    }

    raw_packet &operator=(raw_packet &&other) noexcept
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