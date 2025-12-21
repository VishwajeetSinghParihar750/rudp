#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <algorithm>

#include "client/client_setup.hpp"

static constexpr size_t TOTAL_BYTES = 1ull * 1024 * 1024 * 1024; // 1 GB
static constexpr size_t BUF_SIZE = 32 * 1024;
static constexpr channel_id CH_ID = 1;

/* ---------------- TIMESTAMP ---------------- */
// static std::string ts()
// {
//     using namespace std::chrono;
//     auto now = system_clock::now();
//     auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

//     std::time_t t = system_clock::to_time_t(now);
//     std::tm tm = *std::localtime(&t);

//     std::ostringstream oss;
//     oss << std::put_time(&tm, "%H:%M:%S")
//         << "." << std::setw(3) << std::setfill('0') << ms.count();
//     return oss.str();
// }

/* ---------------- FAST STREAMING HASH (FNV-1a) ---------------- */
static inline uint64_t hash_byte(uint64_t h, char b)
{
    h ^= static_cast<unsigned char>(b);
    h *= 1099511628211ull;
    return h;
}

int main()
{
    auto client = create_client("127.0.0.1", "9000");
    client->add_channel(CH_ID, channel_type::RELIABLE_ORDERED_CHANNEL);

    std::vector<char> buf(BUF_SIZE);

    std::atomic<size_t> sent{0};
    std::atomic<size_t> received{0};

    uint64_t send_hash = 1469598103934665603ull;
    uint64_t recv_hash = 1469598103934665603ull;

    // std::cout << ts() << " [Client] START 1GB send + echo\n";

    /* ---------------- RECEIVER THREAD ---------------- */
    // std::jthread receiver([&]
    //                       {
    //     auto last_log = std::chrono::steady_clock::now();
    //     size_t last   = 0;

    //     while (received.load(std::memory_order_relaxed) < TOTAL_BYTES)
    //     {
    //         channel_id ch;
    //         ssize_t n = client->read_from_channel_blocking(
    //             ch,
    //             buf.data(),
    //             buf.size());

    //        if (n > 0    ) {

    //         for (ssize_t i = 0; i < n; ++i)
    //             recv_hash = hash_byte(recv_hash, buf[i]);

    //         received.fetch_add(n, std::memory_order_relaxed);

    //        }
    //         auto now = std::chrono::steady_clock::now();
    //         if (now - last_log >= std::chrono::seconds(1))
    //         {
    //             size_t cur   = received.load();
    //             size_t delta = cur - last;

    //             std::cout
    //                 // << ts()
    //                 << " [Client][RECV][STATS] "
    //                 << (delta / (1024.0 * 1024.0))
    //                 << " MB/s" << std::endl;

    //             last     = cur;
    //             last_log = now;
    //         }
    //     }

    //     std::cout  << " [Client][RECV] DONE" << std::endl; });

    /* ---------------- SENDER (MAIN THREAD) ---------------- */
    auto last_log = std::chrono::steady_clock::now();
    size_t last = 0;

    while (sent.load(std::memory_order_relaxed) < TOTAL_BYTES)
    {
        size_t offset = sent.load();
        size_t chunk = std::min(BUF_SIZE, TOTAL_BYTES - offset);

        // generate deterministic data on the fly
        for (size_t i = 0; i < chunk; ++i)
        {
            char v = static_cast<char>((offset + i) * 1315423911ull);
            buf[i] = v;
            send_hash = hash_byte(send_hash, v);
        }

        ssize_t n = client->write_to_channel(
            CH_ID,
            buf.data(),
            chunk);
        // std::this_thread::sleep_for(duration_ms(1));
        if (n > 0)
        {
            sent.fetch_add(n, std::memory_order_relaxed);
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1))
        {
            size_t cur = sent.load();
            size_t delta = cur - last;

            std::cout
                << "total : " << cur
                << ", [Client][SEND][STATS] "
                << (delta / (1024.0 * 1024.0))
                << " MB/s" << std::endl;

            last = cur;
            last_log = now;
        }
    }

    std::cout
        // << ts()
        << " [Client][SEND] DONE total="
        << (sent.load() / (1024.0 * 1024.0))
        << " MB" << std::endl;

    // receiver.join();

    /* ---------------- VERIFY ---------------- */
    // std::cout << ts() << " [Client] verifying...\n";

    if (send_hash != recv_hash)
    {
        std::cerr
            // << ts()
            << " [FATAL] HASH MISMATCH\n"
            << "send_hash=0x" << std::hex << send_hash << "\n"
            << "recv_hash=0x" << std::hex << recv_hash << "\n";
        return 1;
    }

    std::cout << " [Client] ✅ DATA VERIFIED (1GB OK)\n";
    return 0;
}
