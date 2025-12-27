#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

using namespace std;

// --- CONFIGURATION ---
const string SERVER_BIN = "./rudp_server";
const string CLIENT_BIN = "./rudp_client";
const string TCP_BIN = "./tcp_benchmark";
const string SERVER_IP = "127.0.0.1";
const int DURATION = 10;

struct Scenario
{
    string name;
    int loss_percent;
    int delay_ms;
};

// --- HELPER FUNCTIONS ---

// Execute a shell command silently (unless it fails)
void exec_cmd(const string &cmd)
{
    int ret = system(cmd.c_str());
    if (ret != 0)
    {
        if (cmd.find("del") == string::npos && cmd.find("pkill") == string::npos)
        {
            cerr << "⚠️ Command failed: " << cmd << endl;
        }
    }
}

// Set network conditions using Linux Traffic Control (tc)
void set_network(int loss, int delay)
{
    exec_cmd("sudo tc qdisc del dev lo root 2>/dev/null"); // Clear old
    if (loss == 0 && delay == 0)
        return;

    stringstream cmd;
    cmd << "sudo tc qdisc add dev lo root netem";
    if (loss > 0)
        cmd << " loss " << loss << "%";
    if (delay > 0)
        cmd << " delay " << delay << "ms";

    exec_cmd(cmd.str());
}

// Parse the temporary log file to find "SPEED: X MB/s"
double parse_speed(const string &log_file)
{
    ifstream infile(log_file);
    string line;
    double speed = 0.0;
    while (getline(infile, line))
    {
        if (line.find("SPEED") != string::npos)
        {
            size_t colon_pos = line.find(":");
            if (colon_pos != string::npos)
            {
                try {
                    speed = stod(line.substr(colon_pos + 1));
                } catch (...) { }
            }
        }
    }
    return speed;
}

// Run a specific protocol (TCP or HELIOS)
double run_test_pass(const string &protocol, const string &log_file)
{
    // 1. Cleanup
    exec_cmd("pkill -9 rudp_server 2>/dev/null");
    exec_cmd("pkill -9 rudp_client 2>/dev/null");
    exec_cmd("pkill -9 tcp_benchmark 2>/dev/null");

    // 2. Start Server in Background
    string server_cmd;
    if (protocol == "TCP")
        server_cmd = TCP_BIN + " server > " + log_file + " 2>&1 &";
    else
        server_cmd = SERVER_BIN + " > " + log_file + " 2>&1 &";

    system(server_cmd.c_str());

    // 3. Wait for Server to Init
    this_thread::sleep_for(chrono::seconds(2));

    // 4. Start Client
    string client_cmd;
    if (protocol == "TCP")
        client_cmd = TCP_BIN + " client " + SERVER_IP + " " + to_string(DURATION) + " > /dev/null";
    else
        client_cmd = CLIENT_BIN + " " + SERVER_IP + " " + to_string(DURATION) + " > /dev/null";

    int ret = system(client_cmd.c_str());

    // 5. Cleanup Server
    exec_cmd("pkill -9 rudp_server 2>/dev/null");
    exec_cmd("pkill -9 tcp_benchmark 2>/dev/null");

    if (ret != 0) return 0.0;
    return parse_speed(log_file);
}

int main()
{
    cout << "==========================================================" << endl;
    cout << " 🧪 HELIOS C++ BENCHMARK SUITE " << endl;
    cout << "==========================================================" << endl;

    // 1. COMPILE
    cout << "[Setup] Compiling..." << endl;
    // Note: Added -std=c++20 to all compiles to be safe
    if (system("g++ -O3 -std=c++20 -pthread tcp_benchmark.cpp -o tcp_benchmark") != 0)
        cerr << "TCP compile fail\n";
    if (system("g++ -O3 -std=c++20 -pthread server_main.cpp -o rudp_server") != 0)
        cerr << "Server compile fail\n";
    if (system("g++ -O3 -std=c++20 -pthread client_main.cpp -o rudp_client") != 0)
        cerr << "Client compile fail\n";

    // 2. DEFINE SCENARIOS
    vector<Scenario> scenarios = {
        {"Ideal Localhost", 0, 0},
        {"Spotty WiFi    ", 1, 0},  // 1% Loss
        {"Cross Country  ", 0, 50}, // 50ms Delay
        {"Congestion     ", 5, 0},  // 5% Loss
        {"The Nightmare  ", 2, 50}  // 2% Loss, 50ms Delay
    };

    cout << "\nRunning " << scenarios.size() << " scenarios...\n" << endl;
    printf("%-20s | %-15s | %-15s | %s\n", "SCENARIO", "TCP SPEED", "HELIOS SPEED", "WINNER");
    printf("--------------------------------------------------------------------------\n");

    // 3. EXECUTE LOOP
    for (const auto &s : scenarios)
    {
        set_network(s.loss_percent, s.delay_ms);
        double tcp_speed = run_test_pass("TCP", "tcp_log.tmp");
        double helios_speed = run_test_pass("HELIOS", "helios_log.tmp");

        string winner = (helios_speed > tcp_speed) ? "🏆 HELIOS" : "TCP";
        if (helios_speed == 0 && tcp_speed == 0) winner = "💀 BOTH DIED";

        printf("%-20s | %7.2f MB/s    | %7.2f MB/s    | %s\n",
               s.name.c_str(), tcp_speed, helios_speed, winner.c_str());
    }

    // 4. CLEANUP
    exec_cmd("sudo tc qdisc del dev lo root 2>/dev/null");
    exec_cmd("rm *.tmp");

    cout << "\n✅ Benchmark Suite Complete." << endl;
    return 0;
}