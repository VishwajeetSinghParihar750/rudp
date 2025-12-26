// bench_tcp_server.cpp
#include <iostream>
#include <vector>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>
#include <chrono>
#include <atomic>
#include <thread>
#include <iomanip>

constexpr int PORT = 9000;
constexpr size_t BUFFER_SIZE = 65536;               // 64KB reads
constexpr int SOCKET_BUFFER_SIZE = 4 * 1024 * 1024; // 4MB Kernel Buffer (Fairness)

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    // 1. Optimize Kernel Buffers (Fair fight with your Slab Allocator)
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int buf_size = SOCKET_BUFFER_SIZE;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind");
        return 1;
    }
    listen(server_fd, 3);

    std::cout << "[TCP Benchmark] Server Listening on 9000..." << std::endl;

    int sock = accept(server_fd, nullptr, nullptr);
    if (sock < 0)
    {
        perror("accept");
        return 1;
    }

    std::cout << "[TCP Benchmark] Client Connected! Starting measurement..." << std::endl;

    std::vector<char> buffer(BUFFER_SIZE);
    size_t total_bytes = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_log = start;

    while (true)
    {
        ssize_t bytes = recv(sock, buffer.data(), BUFFER_SIZE, 0);
        if (bytes <= 0)
            break; // Disconnected
        total_bytes += bytes;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 1)
        {
            auto elapsed_seconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() / 1000.0;
            double mb_s = (total_bytes / (1024.0 * 1024.0)) / elapsed_seconds;

            std::cout << std::fixed << std::setprecision(2)
                      << "[TCP Stats] Throughput: " << mb_s << " MB/s | Total: "
                      << (total_bytes / (1024.0 * 1024.0)) << " MB" << std::endl;
            last_log = now;
        }
    }

    close(sock);
    close(server_fd);
    std::cout << "[TCP Benchmark] Finished." << std::endl;
    return 0;
}