
#pragma once

#include <netdb.h>
#include <sys/socket.h>
#include <chrono>
#include <inttypes.h>
#include <random>

enum class channel_type
{
};

inline const std::mt19937_64 rng((int64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
inline std::uniform_int_distribution<int64_t> uid(0ll, UINT64_MAX);

inline uint64_t get_random_channel_id()
{
    return uid(rng);
}

using channel_id = uint64_t;
using client_id = uint64_t;
inline constexpr channel_id invalid_channel_id = 0;
inline constexpr client_id invalid_client_id = 0;

class i_sockaddr
{

public:
    virtual ~i_sockaddr() = default;

    virtual const sockaddr *get_sockaddr() const = 0;
    virtual socklen_t get_socklen() const = 0;

    virtual sockaddr *get_mutable_sockaddr() = 0;
    virtual socklen_t *get_mutable_socklen() = 0;
};