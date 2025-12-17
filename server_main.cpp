// test_server_verification.cpp
#include "server/server_setup.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <map>
#include <atomic>
#include <mutex>

class ServerMonitor
{
private:
    std::map<channel_id, std::map<client_id, size_t>> channel_client_stats;
    std::mutex stats_mutex;
    std::atomic<size_t> total_bytes_received{0};
    std::atomic<size_t> total_messages_received{0};

public:
    void record_message(channel_id ch, client_id cl, size_t bytes)
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        channel_client_stats[ch][cl] += bytes;
        total_bytes_received += bytes;
        total_messages_received++;
    }

    void print_stats()
    {
        std::lock_guard<std::mutex> lock(stats_mutex);

        std::cout << "\n=== Server Statistics ===" << std::endl;
        std::cout << "Total Messages: " << total_messages_received << std::endl;
        std::cout << "Total Bytes: " << total_bytes_received << std::endl;
        std::cout << "Active Channels: " << channel_client_stats.size() << std::endl;

        for (const auto &[channel, clients] : channel_client_stats)
        {
            std::cout << "\nChannel " << channel << ":" << std::endl;
            for (const auto &[client, bytes] : clients)
            {
                std::cout << "  Client " << client << ": " << bytes << " bytes" << std::endl;
            }
        }
    }
};

int main()
{
    std::cout << "=== Server Verification Test ===" << std::endl;

    auto server = create_server("3003");
    ServerMonitor monitor;

    server->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);
    server->add_channel(2, channel_type::ORDERED_UNRELIABLE_CHANNEL);
    server->add_channel(3, channel_type::UNORDERED_UNRELIABLE_CHANNEL);
    std::cout << "Registered 1000 channels" << std::endl;

    std::atomic<bool> running{true};

    // Statistics thread
    std::jthread stats_thread([&]()
                              {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            monitor.print_stats();
        } });

    // Main processing loop
    try
    {
        while (true)
        {
            char buffer[65535];
            channel_id ch_id;
            client_id cl_id;

            // Blocking read
            ssize_t nread = server->read_from_channel_blocking(ch_id, cl_id, buffer, sizeof(buffer));

            if (nread > 0)
            {
                // Record statistics
                monitor.record_message(ch_id, cl_id, nread);

                // Echo back (simple verification)
                server->write_to_channel(ch_id, cl_id, buffer, nread);

                // Log interesting patterns
                // if (nread < 10)
                // {
                //     std::string msg(buffer, nread);
                //     std::cout << "[DEBUG] Short message on ch" << ch_id << " from cl" << cl_id << ": " << msg << std::endl;
                // }
            }
            else if (nread < 0)
            {
                std::cerr << "[ERROR] Read error: " << nread << std::endl;
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Server error: " << e.what() << std::endl;
    }

    running = false;
    if (stats_thread.joinable())
    {
        stats_thread.join();
    }

    server->close_server();
    return 0;
}