#include "client/client_setup.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <mutex>

// --- CONFIGURATION ---
const int TEST_DURATION_SEC = 30;
const int PACKET_SIZE = 1024; // 1KB Payload

struct Payload
{
    uint32_t seq;
    long long timestamp_ns; // Nanoseconds
    char junk[PACKET_SIZE];
};

// --- STATS CONTAINER ---
struct Stats
{
    std::atomic<size_t> sent_packets{0};
    std::atomic<size_t> recv_packets{0};
    std::atomic<size_t> bytes_recv{0};
    std::vector<long long> latencies; // RTT in microseconds
    std::mutex lat_mtx;
} stats;

std::atomic<bool> running{true};

void receiver_thread(std::shared_ptr<i_client> client)
{
    char buf[65535];
    channel_id ch;

    while (running)
    {
        ssize_t n = client->read_from_channel_blocking(ch, buf, sizeof(buf));
        if (n >= static_cast<ssize_t>(sizeof(uint32_t) + sizeof(long long)))
        {
            auto *p = reinterpret_cast<Payload *>(buf);

            auto now = std::chrono::steady_clock::now();
            long long now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now.time_since_epoch())
                    .count();

            long long rtt_us = (now_ns - p->timestamp_ns) / 1000;

            stats.recv_packets++;
            stats.bytes_recv += n;

            if (stats.recv_packets % 10 == 0)
            {
                std::lock_guard<std::mutex> lock(stats.lat_mtx);
                stats.latencies.push_back(rtt_us);
            }
        }
    }
}

void sender_thread(std::shared_ptr<i_client> client, int wait_time_us)
{
    uint32_t seq = 0;
    std::vector<char> raw_buf(sizeof(Payload));
    auto *p = reinterpret_cast<Payload *>(raw_buf.data());

    std::memset(p->junk, 'X', PACKET_SIZE);

    while (running)
    {
        p->seq = ++seq;
        auto now = std::chrono::steady_clock::now();
        p->timestamp_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch())
                .count();

        client->write_to_channel(1, raw_buf.data(), raw_buf.size());
        stats.sent_packets++;

        if (wait_time_us > 0)
            std::this_thread::sleep_for(
                std::chrono::microseconds(wait_time_us));
    }
}

int main(int argc, char *argv[])
{
    int wait_time_us = 0;

    if (argc >= 2)
    {
        wait_time_us = std::stoi(argv[1]);
        std::cout << "Sender wait time: " << wait_time_us << " us\n";
    }

    std::cout << "=== RUDP ULTIMATE STRESS TEST ===\n";

    auto client = create_client("127.0.0.1", "3003");
    client->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);

    std::thread r_thread(receiver_thread, client);
    std::thread s_thread(sender_thread, client, wait_time_us);

    std::cout << "Running test for " << TEST_DURATION_SEC << " seconds...\n";

    for (int i = 0; i < TEST_DURATION_SEC; i++)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "\r[Running] Sent: " << stats.sent_packets
                  << " | Recv: " << stats.recv_packets << std::flush;
    }

    running = false;
    client->close_client();

    if (r_thread.joinable())
        r_thread.join();
    if (s_thread.joinable())
        s_thread.join();

    // --- FINAL REPORT ---
    std::cout << "\n\n=== PERFORMANCE REPORT ===\n";
    std::cout << "Total Sent:     " << stats.sent_packets << "\n";
    std::cout << "Total Recv:     " << stats.recv_packets << "\n";

    double loss =
        100.0 * (1.0 -
                 (double)stats.recv_packets /
                     std::max<size_t>(1, stats.sent_packets));

    std::cout << "Packet Loss:    " << std::fixed << std::setprecision(2)
              << loss << "%\n";

    double total_mb =
        static_cast<double>(stats.bytes_recv) / (1024.0 * 1024.0);
    double throughput = total_mb / TEST_DURATION_SEC;

    std::cout << "Avg Throughput: " << throughput << " MiB/s\n";

    std::vector<long long> lats;
    {
        std::lock_guard<std::mutex> lock(stats.lat_mtx);
        lats = stats.latencies;
    }

    if (!lats.empty())
    {
        std::sort(lats.begin(), lats.end());

        long long avg = 0;
        for (auto v : lats)
            avg += v;
        avg /= lats.size();

        std::cout << "Latency (RTT):\n";
        std::cout << "  Min: " << lats.front() << " us\n";
        std::cout << "  Avg: " << avg << " us\n";
        std::cout << "  p95: " << lats[lats.size() * 95 / 100] << " us\n";
        std::cout << "  p99: " << lats[lats.size() * 99 / 100] << " us\n";
        std::cout << "  Max: " << lats.back() << " us\n";
    }
    else
    {
        std::cout << "Latency: No packets received.\n";
    }

    return 0;
}
