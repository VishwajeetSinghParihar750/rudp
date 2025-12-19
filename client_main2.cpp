#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <chrono>

#include "client/client_setup.hpp"

static constexpr size_t TOTAL_BYTES = 10 * 1024 * 1024; // 10 MB
static constexpr size_t BUF_SIZE = 1 * 1024;
static constexpr channel_id CH_ID = 1;

int main()
{
    auto client = create_client("127.0.0.1", "9000");
    client->add_channel(CH_ID, channel_type::RELIABLE_ORDERED_CHANNEL);

    std::vector<char> send_buf(TOTAL_BYTES);
    std::vector<char> recv_buf(TOTAL_BYTES);

    for (size_t i = 0; i < TOTAL_BYTES; ++i)
        send_buf[i] = static_cast<char>((i * 1315423911ULL) & 0xFF);

    /* ---------------- SEND ---------------- */
    size_t sent = 0;
    auto last_log = std::chrono::steady_clock::now();

    while (sent < TOTAL_BYTES)
    {
        size_t chunk = std::min(BUF_SIZE, TOTAL_BYTES - sent);

        ssize_t n = client->write_to_channel(
            CH_ID,
            send_buf.data() + sent,
            chunk);
        std::this_thread::sleep_for(duration_ms(10));
        if (n < 0)
        {
            std::cerr << "[Client] write failed\n";
            return 1;
        }

        sent += n;

        auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1))
        {
            std::cout
                << "[Client][SEND] "
                << sent << " / " << TOTAL_BYTES
                << " bytes\n";
            last_log = now;
        }
    }

    std::cout << "[Client] Finished sending " << sent << " bytes\n";

    /* ---------------- RECEIVE ---------------- */
    size_t received = 0;
    last_log = std::chrono::steady_clock::now();

    while (received < TOTAL_BYTES)
    {
        channel_id ch;

        ssize_t n = client->read_from_channel_blocking(
            ch,
            recv_buf.data() + received,
            TOTAL_BYTES - received);

        if (n <= 0)
        {
            std::cerr << "[Client] read failed\n";
            return 1;
        }

        received += n;

        auto now = std::chrono::steady_clock::now();
        // if (now - last_log >= std::chrono::seconds(1))
        // {
        std::cout
            << "[Client][RECV] "
            << received << " / " << TOTAL_BYTES
            << " bytes\n";
        last_log = now;
        // }
    }

    std::cout << "[Client] Finished receiving " << received << " bytes\n";

    /* ---------------- VERIFY ---------------- */
    for (size_t i = 0; i < TOTAL_BYTES; ++i)
    {
        if (send_buf[i] != recv_buf[i])
        {
            std::cerr
                << "[FATAL] Mismatch at byte " << i
                << " expected=" << int((unsigned char)send_buf[i])
                << " got=" << int((unsigned char)recv_buf[i])
                << "\n";
            return 1;
        }
    }

    std::cout << "[Client] ✅ Data verified successfully\n";
    return 0;
}
