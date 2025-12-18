#include "server/server_setup.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <iomanip>

// --- HIGH PERFORMANCE ECHO SERVER ---

struct Packet
{
    channel_id ch;
    client_id cl;
    std::vector<char> data;
};

// A thread-safe queue for buffering echoes (Producer-Consumer)
class PacketQueue
{
    std::deque<Packet> queue;
    std::mutex mtx;
    std::condition_variable cv;

public:
    void push(channel_id ch, client_id cl, const char *buf, size_t len)
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            // Optimization: Avoid vector re-allocation if possible
            queue.push_back({ch, cl, std::vector<char>(buf, buf + len)});
        }
        cv.notify_one();
    }

    bool pop(Packet &p)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]
                { return !queue.empty(); });
        p = std::move(queue.front());
        queue.pop_front();
        return true;
    }
};

PacketQueue echo_queue;
std::atomic<size_t> total_bytes{0};
std::atomic<size_t> total_msgs{0};

void echo_worker(std::shared_ptr<i_server> server)
{
    Packet p;
    while (true)
    {
        if (echo_queue.pop(p))
        {
            // This is the bottleneck: Check how long this takes
            server->write_to_channel(p.ch, p.cl, p.data.data(), p.data.size());
        }
    }
}

int main()
{
    std::cout << "=== HIGH-PERFORMANCE ECHO SERVER ===" << std::endl;
    auto server = create_server("3003");

    // Register Channels
    server->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);
    server->add_channel(2, channel_type::ORDERED_UNRELIABLE_CHANNEL);
    server->add_channel(3, channel_type::UNORDERED_UNRELIABLE_CHANNEL);

    // Start Echo Worker (Consumer)
    std::thread worker(echo_worker, server);
    worker.detach();

    // Stats Printer
    std::thread stats([]
                      {
        size_t last_bytes = 0;
        while(true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            size_t current = total_bytes.load();
            size_t diff = current - last_bytes;
            double mbs = (double)diff / (1024 * 1024);
            std::cout << "[Server] Throughput: " << std::fixed << std::setprecision(2) << mbs << " MiB/s | Msgs: " << total_msgs.load() << std::endl;
            last_bytes = current;
        } });
    stats.detach();

    // Main Reader Loop (Producer)
    // Allocating buffer ONCE to save CPU
    static char buffer[65535];
    channel_id ch;
    client_id cl;

    std::cout << "Server Ready. Waiting for packets..." << std::endl;

    while (true)
    {
        // Blocking read is fine because write happens in another thread!
        ssize_t n = server->read_from_channel_blocking(ch, cl, buffer, sizeof(buffer));
        if (n > 0)
        {
            echo_queue.push(ch, cl, buffer, n);
            total_bytes += n;
            total_msgs++;
        }
    }

    return 0;
}