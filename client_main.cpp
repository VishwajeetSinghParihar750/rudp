#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <thread>
#include <iomanip>
#include <sstream>

#include "client/client_setup.hpp"

static constexpr size_t TOTAL_BYTES = 200 * 1024; // 10 MB
static constexpr size_t BUF_SIZE = 1 * 1024;
static constexpr channel_id CH_ID = 1;

/* ---------------- TIMESTAMP ---------------- */
static std::string ts()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::time_t t = system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

int main()
{
    auto client = create_client("127.0.0.1", "9000");
    client->add_channel(CH_ID, channel_type::RELIABLE_ORDERED_CHANNEL);

    std::vector<char> send_buf(TOTAL_BYTES);
    std::vector<char> recv_buf(TOTAL_BYTES);

    for (size_t i = 0; i < TOTAL_BYTES; ++i)
        send_buf[i] = static_cast<char>((i * 1315423911ULL) & 0xFF);

    std::atomic<size_t> received{0};
    std::atomic<bool> recv_done{false};

    std::cout << ts() << " [Client] START sending + receiving\n";

    /* ---------------- RECEIVER THREAD ---------------- */
    // std::jthread reader([&]
    //                     {
    //     size_t last_bytes = 0;
    //     auto last_log = std::chrono::steady_clock::now();

    //     while (received.load(std::memory_order_relaxed) < TOTAL_BYTES)
    //     {
    //         channel_id ch;
    //         size_t offset = received.load(std::memory_order_relaxed);

    //         std::cout
    //             << ts()
    //             << " [Client][RECV] waiting... offset="
    //             << offset << "\n";

    //         ssize_t n = client->read_from_channel_blocking(
    //             ch,
    //             recv_buf.data() + offset,
    //             TOTAL_BYTES - offset);

    //         if (n <= 0)
    //         {
    //             std::cerr
    //                 << ts()
    //                 << " [Client][ERROR][RECV] read failed n="
    //                 << n << "\n";
    //             std::terminate();
    //         }

    //         received.fetch_add(n, std::memory_order_relaxed);

    //         std::cout
    //             << ts()
    //             << " [Client][RECV] channel="
    //             << ch
    //             << " bytes="
    //             << n
    //             << " total_received="
    //             << received.load()
    //             << "\n";

    //         auto now = std::chrono::steady_clock::now();
    //         if (now - last_log >= std::chrono::seconds(1))
    //         {
    //             size_t cur = received.load();
    //             size_t delta = cur - last_bytes;

    //             std::cout
    //                 << ts()
    //                 << " [Client][RECV][STATS] "
    //                 << delta << " bytes/sec ("
    //                 << std::fixed << std::setprecision(2)
    //                 << (delta / 1024.0 / 1024.0)
    //                 << " MB/s)\n";

    //             last_bytes = cur;
    //             last_log = now;
    //         }
    //     }

    //     recv_done.store(true, std::memory_order_release);
    //     std::cout << ts() << " [Client][RECV] DONE\n"; });

    /* ---------------- SENDER (MAIN THREAD) ---------------- */
    size_t sent = 0;
    size_t last_sent = 0;
    auto last_log = std::chrono::steady_clock::now();

    while (sent < TOTAL_BYTES)
    {
        size_t chunk = std::min(BUF_SIZE, TOTAL_BYTES - sent);

        std::cout
            << ts()
            << " [Client][SEND] attempt chunk="
            << chunk
            << " offset="
            << sent
            << "\n";

        ssize_t n = client->write_to_channel(
            CH_ID,
            send_buf.data() + sent,
            chunk);

        std::this_thread::sleep_for(duration_ms(100));
        if (n < 0)
        {
            std::cerr
                << ts()
                << " [Client][ERROR][SEND] write failed\n";
            return 1;
        }

        sent += n;

        std::cout
            << ts()
            << " [Client][SEND] wrote="
            << n
            << " total_sent="
            << sent
            << "\n";

        auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1))
        {
            size_t delta = sent - last_sent;

            std::cout
                << ts()
                << " [Client][SEND][STATS] "
                << delta << " bytes/sec ("
                << std::fixed << std::setprecision(2)
                << (delta / 1024.0 / 1024.0)
                << " MB/s)\n";

            last_sent = sent;
            last_log = now;
        }
    }

    std::cout
        << ts()
        << " [Client][SEND] DONE total="
        << sent
        << " bytes\n";

    /* ---------------- WAIT FOR RECEIVER ---------------- */
    while (!recv_done.load(std::memory_order_acquire))
    {
        std::cout
            << ts()
            << " [Client][WAIT] receiver still running, received="
            << received.load()
            << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout
        << ts()
        << " [Client] FINISHED recv="
        << received
        << " bytes\n";

    /* ---------------- VERIFY ---------------- */
    for (size_t i = 0; i < TOTAL_BYTES; ++i)
    {
        if (send_buf[i] != recv_buf[i])
        {
            std::cerr
                << ts()
                << " [FATAL] mismatch at byte "
                << i
                << " expected="
                << int((unsigned char)send_buf[i])
                << " got="
                << int((unsigned char)recv_buf[i])
                << "\n";
            return 1;
        }
    }

    std::cout
        << ts()
        << " [Client] ✅ DATA VERIFIED SUCCESSFULLY\n";

    return 0;
}
