#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>

#include "server/server_setup.hpp"

static constexpr size_t TOTAL_BYTES = 1000 * 1024 * 1024;
static constexpr size_t BUF_SIZE = 64 * 1024;

int main()
{
    auto server = create_server("9000");
    server->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);

    std::cout << "[Server] Listening...\n";

    size_t total = 0;
    std::vector<char> buf(BUF_SIZE);

    auto last_log = std::chrono::steady_clock::now();

    while (true)
    {
        // channel_id ch;
        // client_id cl;

        // ssize_t n = server->read_from_channel_blocking(
        //     ch,
        //     cl,
        //     buf.data(),
        //     buf.size());

        // if (n < 0)
        // {
        //     std::cerr << "[Server] read failed\n";
        //     return 1;
        // }

        // size_t off = 0;
        // while (off < static_cast<size_t>(n))
        // {
        //     ssize_t w = server->write_to_channel(
        //         ch,
        //         cl,
        //         buf.data() + off,
        //         n - off);

        //     if (w <= 0)
        //     {
        //         std::cerr << "[Server] write failed\n";
        //         return 1;
        //     }

        //     off += w;
        // }

        // total += n;

        // auto now = std::chrono::steady_clock::now();
        // if (now - last_log >= std::chrono::seconds(1))
        // {
        //     std::cout
        //         << "[Server] Echoed "
        //         << total << " / " << TOTAL_BYTES
        //         << " bytes\n";
        //     last_log = now;
        // }
        std::this_thread::sleep_for(duration_ms(1000));
    }

    std::cout << "[Server] ✅ Echoed " << total << " bytes successfully\n";
    std::this_thread::sleep_for(duration_ms(10));
    return 0;
}
