#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <thread>

#include "server/server_setup.hpp"

static constexpr size_t BUF_SIZE = 64 * 1024;

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

    std::cout << ts() << " [Server] START Listening on port 9000\n";

    size_t total = 0;
    std::vector<char> buf(BUF_SIZE);

    auto last_log = std::chrono::steady_clock::now();
    size_t bytes_last_sec = 0;

    while (true)
    {
        channel_id ch;
        client_id cl;

        ssize_t n = server->read_from_channel_nonblocking(
            ch,
            cl,
            buf.data(),
            buf.size());

        if (n < -2)
        {
            std::cerr
                << ts()
                << " [Server][ERROR] read failed, code="
                << n << "\n";
            return 1;
        }

        if (n == -2)
        {
            std::cout
                << ts()
                << " [Server][READ] no data available\n";
        }
        else if (n == 0)
        {
            std::cout
                << ts()
                << " [Server][READ] connection closed? n=0\n";
        }
        else
        {
            std::cout
                << ts()
                << " [Server][READ] client="
                << cl
                << " channel="
                << ch
                << " bytes="
                << n
                << "\n";

            size_t off = 0;
            while (off < static_cast<size_t>(n))
            {
                ssize_t w = server->write_to_channel(
                    ch,
                    cl,
                    buf.data() + off,
                    n - off);

                if (w < 0)
                {
                    std::cerr
                        << ts()
                        << " [Server][ERROR] write failed\n";
                    return 1;
                }

                std::cout
                    << ts()
                    << " [Server][WRITE] client="
                    << cl
                    << " channel="
                    << ch
                    << " wrote="
                    << w
                    << " remaining="
                    << (n - off - w)
                    << "\n";

                off += w;
            }

            total += n;
            bytes_last_sec += n;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1))
        {
            double mbps =
                (bytes_last_sec / 1024.0 / 1024.0);

            std::cout
                << ts()
                << " [Server][STATS] total="
                << total
                << " bytes, last_sec="
                << bytes_last_sec
                << " bytes ("
                << std::fixed << std::setprecision(2)
                << mbps
                << " MB/s)\n";

            bytes_last_sec = 0;
            last_log = now;
        }

        std::this_thread::sleep_for(duration_ms(1000));
    }
}
