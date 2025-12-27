#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <iomanip>
#include "client/client_setup.hpp"

// Shared flag to stop threads
std::atomic<bool> keep_sending{true};

void send_worker(std::shared_ptr<i_client> client, channel_id ch, size_t buf_size, std::atomic<size_t> &global_sent)
{
    // 4KB Chunks to be safe with Slab Allocator
    std::vector<char> buf(buf_size, 'B'); 
    
    while (keep_sending)
    {
        ssize_t n = client->write_to_channel(ch, buf.data(), buf.size());
        if (n > 0) global_sent.fetch_add(n, std::memory_order_relaxed);
        else std::this_thread::yield();
    }
}

int main(int argc, char* argv[])
{
    // 1. READ IP FROM ARGUMENT (Position 1)
    std::string server_ip = (argc > 1) ? argv[1] : "127.0.0.1";
    
    // 2. READ DURATION FROM ARGUMENT (Position 2)
    int TEST_DURATION_SEC = (argc > 2) ? std::atoi(argv[2]) : 10;

    std::cout << "🚀 Connecting to " << server_ip << " for " << TEST_DURATION_SEC << "s..." << std::endl;

    auto client = create_client(server_ip.c_str(), "9005");

    client->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);
    client->add_channel(2, channel_type::ORDERED_UNRELIABLE_CHANNEL);
    client->add_channel(3, channel_type::UNORDERED_UNRELIABLE_CHANNEL);

    // Initial ping to ensure connection before starting timer
    char ping[] = "ping";
    std::cout << "⏳ Handshaking..." << std::endl;
    while(client->write_to_channel(1, ping, 4) <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "✅ Connected!" << std::endl;

    std::atomic<size_t> total_sent{0};
    auto start_time = std::chrono::steady_clock::now();

    // Launch Workers
    std::thread t1(send_worker, client, 1, 16 * 1024, std::ref(total_sent));
    std::thread t2(send_worker, client, 2, 32 * 1024, std::ref(total_sent));
    std::thread t3(send_worker, client, 3, 32 * 1024, std::ref(total_sent));

    // Main thread acts as the timer
    std::this_thread::sleep_for(std::chrono::seconds(TEST_DURATION_SEC));

    // Stop threads
    keep_sending = false;
    if (t1.joinable()) t1.join();
    if (t2.joinable()) t2.join();
    if (t3.joinable()) t3.join();

    auto end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;
    double final_mb = total_sent.load() / (1024.0 * 1024.0);

    // FORMAT FOR SCRIPT PARSING (Backup, server logs usually preferred)
    std::cout << "Final Avg Speed: " << (final_mb / diff.count()) << " MB/s" << std::endl;

    return 0;
}