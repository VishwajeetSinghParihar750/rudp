/* tcp_benchmark.cpp 
   Compile: g++ -o tcp_benchmark tcp_benchmark.cpp -O3
*/

#include <iostream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 65536 // 64KB buffer
#define PORT 8080

using namespace std;
using namespace std::chrono;

// Global flag for clean shutdown
atomic<bool> keep_running(true);

void signal_handler(int signum) {
    keep_running = false;
}

// ---------------------------------------------------------
// SERVER MODE
// ---------------------------------------------------------
void run_server() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // FORCE reuse address/port (Fixes "Address already in use")
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    cout << "[TCP-Server] Listening on " << PORT << "..." << endl;

    if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
        perror("Accept failed");
        exit(EXIT_FAILURE);
    }

    // Benchmark Loop
    char buffer[BUFFER_SIZE];
    long long total_bytes = 0;
    auto start_time = high_resolution_clock::now();
    bool first_packet = true;

    while (keep_running) {
        int valread = read(new_socket, buffer, BUFFER_SIZE);
        if (valread <= 0) break; // EOF or Error

        if (first_packet) {
            start_time = high_resolution_clock::now();
            first_packet = false;
        }
        total_bytes += valread;
    }

    auto end_time = high_resolution_clock::now();
    duration<double> elapsed = end_time - start_time;
    double mb_s = (total_bytes / (1024.0 * 1024.0)) / elapsed.count();

    cout << "[TCP-Server] Received " << total_bytes << " bytes in " << elapsed.count() << "s." << endl;
    cout << "SPEED: " << mb_s << " MB/s" << endl;

    close(new_socket);
    close(server_fd);
}

// ---------------------------------------------------------
// CLIENT MODE
// ---------------------------------------------------------
void run_client(const char* ip, int duration_sec) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE];
    memset(buffer, 'A', BUFFER_SIZE); // Fill garbage data

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return;
    }

    // RETRY LOGIC (Fixes "FATAL: Could not connect")
    bool connected = false;
    for (int i = 0; i < 5; i++) {
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) >= 0) {
            connected = true;
            break;
        }
        cout << "[TCP-Client] Connection failed, retrying in 1s..." << endl;
        this_thread::sleep_for(seconds(1));
    }

    if (!connected) {
        cerr << "FATAL: Could not connect to TCP server after retries." << endl;
        return;
    }

    cout << "[TCP-Client] Sending data for " << duration_sec << " seconds..." << endl;
    auto start_time = high_resolution_clock::now();

    while (keep_running) {
        if (send(sock, buffer, BUFFER_SIZE, 0) < 0) break;
        
        auto current_time = high_resolution_clock::now();
        duration<double> elapsed = current_time - start_time;
        if (elapsed.count() >= duration_sec) break;
    }

    // Clean shutdown
    shutdown(sock, SHUT_WR); 
    close(sock);
    cout << "[TCP-Client] Done." << endl;
}

int main(int argc, char const *argv[]) {
    signal(SIGINT, signal_handler);

    if (argc < 2) {
        cout << "Usage: ./tcp_benchmark [server|client] [ip (client only)] [duration (client only)]" << endl;
        return 1;
    }

    string mode = argv[1];

    if (mode == "server") {
        run_server();
    } else if (mode == "client") {
        const char* ip = (argc >= 3) ? argv[2] : "127.0.0.1";
        int duration = (argc >= 4) ? stoi(argv[3]) : 10;
        run_client(ip, duration);
    }

    return 0;
}