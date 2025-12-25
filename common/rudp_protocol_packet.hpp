#pragma once
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "i_packet.hpp"
#include "slab_allocator/slab_provider.hpp"

class rudp_protocol_packet final : public i_packet
{
    size_t length;
    size_t capacity;
    char *mem = nullptr;
    uint8_t bucket_id = 0;

    template <uint8_t ID>
    struct bucket_info
    {
        // Helper to get size at compile time
        static constexpr size_t SIZE = std::min(64000ull, 1ULL << (ID + 4));
        using slab = slab_memory<SIZE, "RUDP_BUCKET">;
    };

    static inline uint8_t get_bucket_id(size_t n)
    {
        if (n <= 16)
            return 0;
        uint8_t bit = 64 - __builtin_clzll(n - 1);
        return (bit < 4) ? 0 : (bit - 4);
    }

    void free_mem_to_slab()
    {
        if (!mem)
            return;

        // Calculate size to subtract based on bucket_id
        // We re-calculate size logic here or rely on the template
        size_t size_freed = 0;

        switch (bucket_id)
        {
        case 0:
            bucket_info<0>::slab::free(mem);
            size_freed = bucket_info<0>::SIZE;
            break;
        case 1:
            bucket_info<1>::slab::free(mem);
            size_freed = bucket_info<1>::SIZE;
            break;
        case 2:
            bucket_info<2>::slab::free(mem);
            size_freed = bucket_info<2>::SIZE;
            break;
        case 3:
            bucket_info<3>::slab::free(mem);
            size_freed = bucket_info<3>::SIZE;
            break;
        case 4:
            bucket_info<4>::slab::free(mem);
            size_freed = bucket_info<4>::SIZE;
            break;
        case 5:
            bucket_info<5>::slab::free(mem);
            size_freed = bucket_info<5>::SIZE;
            break;
        case 6:
            bucket_info<6>::slab::free(mem);
            size_freed = bucket_info<6>::SIZE;
            break;
        case 7:
            bucket_info<7>::slab::free(mem);
            size_freed = bucket_info<7>::SIZE;
            break;
        case 8:
            bucket_info<8>::slab::free(mem);
            size_freed = bucket_info<8>::SIZE;
            break;
        case 9:
            bucket_info<9>::slab::free(mem);
            size_freed = bucket_info<9>::SIZE;
            break;
        case 10:
            bucket_info<10>::slab::free(mem);
            size_freed = bucket_info<10>::SIZE;
            break;
        case 11:
            bucket_info<11>::slab::free(mem);
            size_freed = bucket_info<11>::SIZE;
            break;
        case 12:
            bucket_info<12>::slab::free(mem);
            size_freed = bucket_info<12>::SIZE;
            break;
        default:
            break;
        }

        // DECREMENT TRACKER

        mem = nullptr;
    }

    static void *allocate_from_bucket(uint8_t id)
    {
        void *ptr = nullptr;
        size_t size_alloc = 0;

        switch (id)
        {
        case 0:
            ptr = bucket_info<0>::slab::alloc();
            size_alloc = bucket_info<0>::SIZE;
            break;
        case 1:
            ptr = bucket_info<1>::slab::alloc();
            size_alloc = bucket_info<1>::SIZE;
            break;
        case 2:
            ptr = bucket_info<2>::slab::alloc();
            size_alloc = bucket_info<2>::SIZE;
            break;
        case 3:
            ptr = bucket_info<3>::slab::alloc();
            size_alloc = bucket_info<3>::SIZE;
            break;
        case 4:
            ptr = bucket_info<4>::slab::alloc();
            size_alloc = bucket_info<4>::SIZE;
            break;
        case 5:
            ptr = bucket_info<5>::slab::alloc();
            size_alloc = bucket_info<5>::SIZE;
            break;
        case 6:
            ptr = bucket_info<6>::slab::alloc();
            size_alloc = bucket_info<6>::SIZE;
            break;
        case 7:
            ptr = bucket_info<7>::slab::alloc();
            size_alloc = bucket_info<7>::SIZE;
            break;
        case 8:
            ptr = bucket_info<8>::slab::alloc();
            size_alloc = bucket_info<8>::SIZE;
            break;
        case 9:
            ptr = bucket_info<9>::slab::alloc();
            size_alloc = bucket_info<9>::SIZE;
            break;
        case 10:
            ptr = bucket_info<10>::slab::alloc();
            size_alloc = bucket_info<10>::SIZE;
            break;
        case 11:
            ptr = bucket_info<11>::slab::alloc();
            size_alloc = bucket_info<11>::SIZE;
            break;
        case 12:
            ptr = bucket_info<12>::slab::alloc();
            size_alloc = bucket_info<12>::SIZE;
            break;
        default:
            return nullptr;
        }

        return ptr;
    }

public:
    static constexpr size_t SESSION_CONTROL_HEADER_OFFSET = 0;
    static constexpr size_t SESSION_CONTROL_HEADER_SIZE = 5;
    static constexpr size_t CHANNEL_MANAGER_HEADER_OFFSET = 5;
    static constexpr size_t CHANNEL_MANAGER_HEADER_SIZE = 4;
    static constexpr size_t CHANNEL_HEADER_OFFSET = 9;

    explicit rudp_protocol_packet(size_t n) : length(0)
    {
        if (n > 64000)
            throw std::runtime_error("Packet exceeds slab limit");

        bucket_id = get_bucket_id(n);
        capacity = 1ULL << (bucket_id + 4);
        mem = static_cast<char *>(allocate_from_bucket(bucket_id));
    }

    rudp_protocol_packet(size_t n, const char *ibuf) : rudp_protocol_packet(n)
    {
        length = std::min(capacity, n);
        if (ibuf && mem)
        {
            std::copy(ibuf, ibuf + length, mem);
        }
    }

    ~rudp_protocol_packet() override
    {
        free_mem_to_slab();
    }

    rudp_protocol_packet(const rudp_protocol_packet &other)
        : length(other.length), capacity(other.capacity), bucket_id(other.bucket_id)
    {
        mem = static_cast<char *>(allocate_from_bucket(bucket_id));
        if (other.mem && mem)
        {
            std::copy(other.mem, other.mem + other.length, mem);
        }
    }

    rudp_protocol_packet(rudp_protocol_packet &&other) noexcept
        : length(other.length), capacity(other.capacity), mem(other.mem), bucket_id(other.bucket_id)
    {
        other.mem = nullptr;
        other.length = 0;
    }

    rudp_protocol_packet &operator=(const rudp_protocol_packet &other)
    {
        if (this != &other)
        {
            rudp_protocol_packet temp(other);
            *this = std::move(temp);
        }
        return *this;
    }

    rudp_protocol_packet &operator=(rudp_protocol_packet &&other) noexcept
    {
        if (this != &other)
        {
            free_mem_to_slab();

            mem = other.mem;
            length = other.length;
            capacity = other.capacity;
            bucket_id = other.bucket_id;

            other.mem = nullptr;
            other.length = 0;
        }
        return *this;
    }

    char *get_buffer() override { return mem; }
    char *get_const_buffer() const override { return mem; }
    size_t get_capacity() const override { return capacity; }
    size_t get_length() const override { return length; }
    void set_length(size_t newlen) override { length = newlen; }
};