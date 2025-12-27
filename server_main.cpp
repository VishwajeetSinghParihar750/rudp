#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <map>
#include "server/server_setup.hpp"

static constexpr size_t BUF_SIZE = 8 * 1024 * 1024;

int main()
{
    auto server = create_server("9005");

    // Registering the 3 different channel types
    server->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);
    server->add_channel(2, channel_type::ORDERED_UNRELIABLE_CHANNEL);
    server->add_channel(3, channel_type::UNORDERED_UNRELIABLE_CHANNEL);

    std::cout << "[Server] Listening on port 9005 (3 Channels Active)\n";

    std::vector<char> buf(BUF_SIZE);

    // Stats tracking
    std::map<channel_id, size_t> total_per_channel;
    std::map<channel_id, size_t> bytes_this_sec_per_channel;
    size_t grand_total_bytes = 0;

    auto last_tick = std::chrono::steady_clock::now();

    while (true)
    {
        channel_id ch;
        client_id cl;

        // Blocking read returns the channel_id 'ch' for the incoming packet
        ssize_t n = server->read_from_channel_blocking(ch, cl, buf.data(), buf.size());

        if (n > 0)
        {
            total_per_channel[ch] += n;
            bytes_this_sec_per_channel[ch] += n;
            grand_total_bytes += n;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_tick >= std::chrono::seconds(1))
        {
            std::cout << "\n--- [Server Stats] ---\n";

            double total_mb_this_sec = 0.0;

            for (int i = 1; i <= 3; ++i)
            {
                channel_id id = static_cast<channel_id>(i);
                double mbps = bytes_this_sec_per_channel[id] / (1024.0 * 1024.0);
                double total_mb = total_per_channel[id] / (1024.0 * 1024.0);
                
                total_mb_this_sec += mbps;

                std::cout << "Channel " << i << ": "
                          << std::fixed << std::setprecision(2) << mbps << " MB/s "
                          << "(Total: " << total_mb << " MB)\n";

                // Reset per-second counter
                bytes_this_sec_per_channel[id] = 0;
            }

            // CRITICAL FOR BENCHMARK PARSER:
            std::cout << "SPEED: " << total_mb_this_sec << " MB/s" << std::endl;
            std::cout << "Grand Total: " << (grand_total_bytes / (1024.0 * 1024.0)) << " MB\n";
            std::cout << "----------------------\n";

            last_tick = now;
        }
    }
}