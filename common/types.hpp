#pragma once

#include <netdb.h>
#include <sys/socket.h>
#include <chrono>
#include <inttypes.h>
#include <random>
#include <cstdint>

using channel_id = uint64_t;
using client_id = uint64_t;
inline constexpr channel_id INVALID_CHANNEL_ID = 0;
inline constexpr client_id INVALID_CLIENT_ID = 0;
inline constexpr channel_id CONTROL_CHANNEL_ID = 0;

inline constexpr uint16_t MAX_CHANNEL_PACKET_SIZE = 65000;
enum class channel_type : uint16_t
{
    RELIABLE_ORDERED_CHANNEL = 0,
    ORDERED_UNRELIABLE_CHANNEL = 1,
    UNORDERED_UNRELIABLE_CHANNEL = 2,
    RELIABLE_UNORDERED_CHANNEL = 3,
};

static std::mt19937_64 &get_rng()
{
    static const auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    static std::mt19937_64 generator(static_cast<uint64_t>(seed));
    return generator;
}

static std::uniform_int_distribution<uint64_t> &get_uid_distribution()
{
    static std::uniform_int_distribution<uint64_t> distribution(0ull, UINT64_MAX);
    return distribution;
}

inline channel_id get_random_channel_id()
{
    return get_uid_distribution()(get_rng());
}

inline client_id get_random_client_id()
{
    return get_uid_distribution()(get_rng());
}

inline uint64_t get_random_uint64_t()
{
    return get_uid_distribution()(get_rng());
}

inline uint64_t htonll(uint64_t hostval)
{
#if __BYTE_ORDER__ == __BIG_ENDIAN
    return hostval;
#else
    uint32_t l = hostval >> 32, r = (hostval & ((1ll << 32) - 1));
    l = htonl(l), r = htonl(r);
    return (uint64_t(r) << 32) | l;

#endif
}
inline uint64_t ntohll(uint64_t netval) { return htonll(netval); }