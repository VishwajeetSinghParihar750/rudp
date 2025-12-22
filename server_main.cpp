#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>

#include "server/server_setup.hpp"

static constexpr size_t BUF_SIZE = 128 * 1024; // larger buffer = fewer syscalls

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
    auto server = create_server("9000");
    server->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);

    std::cout << ts() << " [Server] Listening on port 9000\n";

    std::vector<char> buf(BUF_SIZE);

    size_t total_bytes = 0;
    size_t bytes_this_sec = 0;

    auto last_tick = std::chrono::steady_clock::now();

    while (true)
    {
        channel_id ch;
        client_id cl;

        ssize_t n = server->read_from_channel_nonblocking(
            ch,
            cl,
            buf.data(),
            buf.size());

        if (n > 0)
        {
            total_bytes += n;
            bytes_this_sec += n;

            // echo back
            // size_t off = 0;
            // while (off < static_cast<size_t>(n))
            // {
            //     ssize_t w = server->write_to_channel(
            //         ch,
            //         cl,
            //         buf.data() + off,
            //         n - off);

            //     if (w > 0)
            //         off += w;
            // }
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_tick >= std::chrono::seconds(1))
        {
            double mbps = bytes_this_sec / (1024.0 * 1024.0);

            std::cout
                // << ts()
                << " [Server][STATS] total="
                << total_bytes
                << " bytes, rate="
                << std::fixed << std::setprecision(2)
                << mbps
                << " MB/s\n";

            bytes_this_sec = 0;
            last_tick = now;
        }

        // Optional: yield a bit to avoid 100% CPU spin
        // std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}
