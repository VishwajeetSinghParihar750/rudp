#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <iomanip>
#include <algorithm>
#include "client/client_setup.hpp"

static constexpr size_t GB = 1024ull * 1024 * 1024;
static constexpr size_t PER_CHANNEL_GOAL = 1ull * 10 * GB;
static constexpr size_t TOTAL_EXPECTED = PER_CHANNEL_GOAL * 1;

// Worker now takes 'buf_size' as a parameter
void send_worker(std::shared_ptr<i_client> client, channel_id ch, size_t bytes_to_send, size_t buf_size, std::atomic<size_t> &global_sent)
{
    // Create a buffer specific to this thread's requirements
    std::vector<char> buf(buf_size, 'A');
    size_t sent_by_thread = 0;

    while (sent_by_thread < bytes_to_send)
    {
        size_t chunk = std::min(buf_size, bytes_to_send - sent_by_thread);
        ssize_t n = client->write_to_channel(ch, buf.data(), chunk);

        if (n > 0)
        {
            sent_by_thread += n;
            global_sent.fetch_add(n, std::memory_order_relaxed);
        }
        else
        {
            // Prevent 100% CPU spin if the OS/Internal buffer is full
            // std::this_thread::yield();

            std::this_thread::sleep_for(duration_ms(1));
        }
    }
    std::cout << "[Client] Channel " << (int)ch << " (Buf: " << buf_size << " bytes) finished sending 1GB.\n";
}

int main()
{
    auto client = create_client("127.0.0.1", "9000");

    // 1. Setup Channels
    client->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);
    // client->add_channel(2, channel_type::ORDERED_UNRELIABLE_CHANNEL);
    // client->add_channel(3, channel_type::UNORDERED_UNRELIABLE_CHANNEL);

    std::atomic<size_t> total_sent{0};
    auto start_time = std::chrono::steady_clock::now();

    // 2. Start 3 Sender Threads with varying buffer sizes
    // Channel 1: 16KB (16384 bytes)
    std::thread t1(send_worker, client, 1, PER_CHANNEL_GOAL, 8 * 1024, std::ref(total_sent));

    // // Channel 2 & 3: 512 Bytes
    // std::thread t2(send_worker, client, 2, PER_CHANNEL_GOAL, 1024, std::ref(total_sent));
    // std::thread t3(send_worker, client, 3, PER_CHANNEL_GOAL, 1024, std::ref(total_sent));

    // 3. Monitor Progress
    while (total_sent < TOTAL_EXPECTED)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        double progress = (total_sent.load() / (double)TOTAL_EXPECTED) * 100.0;
        double sent_mb = total_sent.load() / (1024.0 * 1024.0);

        std::cout << "[Client] Total Progress: " << std::fixed << std::setprecision(2)
                  << progress << "% (" << (int)sent_mb << " MB / 3072 MB)\r" << std::flush;
    }

    t1.join();
    // t2.join();
    // t3.join();

    auto end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;

    std::cout << "\n\n[Client] ALL CHANNELS FINISHED (3GB Total) in " << diff.count() << "s\n";
    std::cout << "Aggregate Throughput: " << (TOTAL_EXPECTED / (1024.0 * 1024.0)) / diff.count() << " MB/s\n";

    return 0;
}