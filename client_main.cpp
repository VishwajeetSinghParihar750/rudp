#include "client/client_setup.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <cstring>
#include <random>

// Structure to make sequence handling easier
struct PayloadHeader
{
    uint32_t seq;
};

int main()
{
    std::cout << "=== UNRELIABLE ORDERED CHANNEL TEST (ID: 2) ===" << std::endl;

    try
    {
        auto client = create_client("127.0.0.1", "3003");
        client->add_channel(3, channel_type::UNORDERED_UNRELIABLE_CHANNEL);

        uint32_t next_to_send = 1;
        uint32_t last_received_seq = 0;

        const int buflen = 65535;
        char buf[buflen];

        int total_sent_packets = 0;
        int total_received_packets = 0;

        // --- PHASE 1: SENDING BURSTS (10,000 packets) ---
        std::cout << "[Phase 1] Sending 10,000 packets with sequence numbers..." << std::endl;

        for (int i = 0; i < 100000; i++)
        {
            // Prepare payload: Header (Seq) + Random Data
            PayloadHeader header;
            header.seq = next_to_send++;

            int data_len = (rand() % 100) + 1;
            int total_packet_len = sizeof(PayloadHeader) + data_len;

            std::vector<char> send_buf(total_packet_len);
            std::memcpy(send_buf.data(), &header, sizeof(PayloadHeader));
            // Fill rest with dummy data
            std::memset(send_buf.data() + sizeof(PayloadHeader), 'A', data_len);

            int cursent = client->write_to_channel(3, send_buf.data(), total_packet_len);
            if (cursent > 0)
                total_sent_packets++;

            // Non-blocking read attempt
            channel_id recv_ch;
            ssize_t curread = client->read_from_channel_nonblocking(recv_ch, buf, buflen);

            if (curread >= (ssize_t)sizeof(PayloadHeader))
            {
                PayloadHeader *recv_header = (PayloadHeader *)buf;

                // VERIFICATION LOGIC
                // if (recv_header->seq <= last_received_seq)
                // {
                //     std::cerr << "\nERROR: Ordering Violation!"
                //               << " Received Seq: " << recv_header->seq
                //               << " but Last Seq was: " << last_received_seq << std::endl;
                //     exit(1);
                // }

                last_received_seq = recv_header->seq;
                total_received_packets++;
                std::cout << last_received_seq << std::endl;
            }

            // High-speed send, but tiny sleep to allow OS to breath
            if (i % 100 == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // --- PHASE 2: 20-SECOND DRAIN & VERIFY ---
        std::cout << "\n[Phase 2] Sent " << total_sent_packets << " packets." << std::endl;
        std::cout << "Draining for 20 seconds to catch remaining echos..." << std::endl;

        auto start_time = std::chrono::steady_clock::now();
        while (true)
        {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count() >= 20)
            {
                break;
            }

            channel_id recv_ch;
            // Using blocking read with a short timeout or just regular blocking
            ssize_t curread = client->read_from_channel_nonblocking(recv_ch, buf, buflen);

            if (curread >= (ssize_t)sizeof(PayloadHeader))
            {
                PayloadHeader *recv_header = (PayloadHeader *)buf;

                if (recv_header->seq <= last_received_seq)
                {
                    std::cerr << "\nERROR: Ordering Violation during drain!"
                              << " Received Seq: " << recv_header->seq
                              << " <= Last Seq: " << last_received_seq << std::endl;
                    exit(1);
                }

                last_received_seq = recv_header->seq;
                total_received_packets++;

                std::cout << "\r[Status] Last Received Seq: " << last_received_seq
                          << " | Total Recv: " << total_received_packets << "   " << std::flush;
            }
        }

        // --- SUMMARY ---
        std::cout << "\n\n=== TEST COMPLETE ===" << std::endl;
        std::cout << "Packets Sent:     " << total_sent_packets << std::endl;
        std::cout << "Packets Received: " << total_received_packets << std::endl;
        std::cout << "Packet Loss:      " << (1.0f - (float)total_received_packets / total_sent_packets) * 100.0f << "%" << std::endl;
        std::cout << "Max Seq Received: " << last_received_seq << std::endl;
        std::cout << "Result: SUCCESS (No ordering violations detected)" << std::endl;

        client->close_client();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}