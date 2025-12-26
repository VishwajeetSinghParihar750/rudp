#include <iostream>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <atomic>
#include <thread>

int main(int argc, char const *argv[]) {
    // READ DURATION FROM ARGUMENT (Default 10s)
    int duration_sec = (argc > 1) ? std::atoi(argv[1]) : 10;

    int sock = 0;
    struct sockaddr_in serv_addr;
    std::vector<char> buffer(1024 * 64, 'A');

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "\n Socket creation error \n";
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        std::cerr << "\nInvalid address/ Address not supported \n";
        return -1;
    }

    // RETRY LOOP
    bool connected = false;
    for (int i = 0; i < 10; ++i) {
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) >= 0) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!connected) {
        std::cerr << "FATAL: Could not connect to TCP server." << std::endl;
        return -1;
    }

    auto start_time = std::chrono::steady_clock::now();
    size_t total_sent = 0;

    // RUN LOOP
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= duration_sec) {
            break;
        }

        ssize_t sent = send(sock, buffer.data(), buffer.size(), 0);
        if (sent > 0) {
            total_sent += sent;
        } else {
            break;
        }
    }

    double mb = total_sent / (1024.0 * 1024.0);
    // PRINT PURE NUMBER FOR SCRIPT PARSING
    std::cout << "Average Speed: " << (mb / duration_sec) << " MB/s" << std::endl;

    close(sock);
    return 0;
}