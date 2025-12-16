// test_comprehensive.cpp
#include "client/client_setup.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <cstring>
#include <random>
#include <algorithm>

// =================== UTILITY FUNCTIONS ===================

class TestLogger
{
public:
    static void info(const std::string &msg)
    {
        std::cout << "[INFO] " << msg << std::endl;
    }

    static void success(const std::string &msg)
    {
        std::cout << "✅ " << msg << std::endl;
    }

    static void error(const std::string &msg)
    {
        std::cout << "❌ " << msg << std::endl;
    }

    static void warning(const std::string &msg)
    {
        std::cout << "⚠️ " << msg << std::endl;
    }
};

// =================== TEST 1: BASIC CHANNEL OPERATIONS ===================
void test_channel_id_reference(std::shared_ptr<i_client> client)
{
    std::cout << "=== Test: Channel ID Reference Behavior ===" << std::endl;

    // Add multiple channels
    const std::vector<channel_id> channels = {100, 101, 102, 103};
    for (auto ch : channels)
    {
        client->add_channel(ch, channel_type::RELIABLE_ORDERED_CHANNEL);
    }

    // Test 1: Blocking read that sets channel_id
    std::cout << "\nTest 1: Blocking read that modifies channel_id" << std::endl;

    std::jthread sender([&](std::stop_token st)
                        {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Send message to channel 102
        std::string msg = "Test message to channel 102";
        ssize_t sent = client->write_to_channel(102, msg.c_str(), msg.size());
        std::cout << "Sent " << sent << " bytes to channel 102" << std::endl; });

    char buffer[1024];
    channel_id received_channel = 100; // Initial value

    std::cout << "Before read: received_channel = " << received_channel << std::endl;

    ssize_t bytes_read = client->read_from_channel_blocking(received_channel,
                                                            buffer, sizeof(buffer));

    std::cout << "After read: received_channel = " << received_channel << std::endl;
    std::cout << "Bytes read: " << bytes_read << std::endl;

    if (bytes_read > 0)
    {
        std::string message(buffer, bytes_read);
        std::cout << "Message from channel " << received_channel << ": "
                  << message << std::endl;

        if (received_channel == 102)
        {
            std::cout << "✅ SUCCESS: Channel ID correctly set to 102" << std::endl;
        }
        else
        {
            std::cout << "❌ FAIL: Expected channel 102, got " << received_channel << std::endl;
        }
    }
}

// Test 2: Multiple channels with non-blocking reads
void test_multiple_channels_with_refs(std::shared_ptr<i_client> client)
{
    std::cout << "\n=== Test 2: Multiple Channel Reading ===" << std::endl;

    // Add test channels
    const int NUM_CHANNELS = 5;
    for (int i = 1; i <= NUM_CHANNELS; i++)
    {
        client->add_channel(i, channel_type::RELIABLE_ORDERED_CHANNEL);
    }

    // Send messages to different channels
    std::jthread sender([&](std::stop_token st)
                        {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Send to channel 3
        client->write_to_channel(3, "Message for channel 3", 21);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        
        // Send to channel 1
        client->write_to_channel(1, "Message for channel 1", 21);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        
        // Send to channel 5
        client->write_to_channel(5, "Message for channel 5", 21); });

    // Read from all channels
    std::map<channel_id, std::string> received_messages;
    int total_reads = 0;
    const int MAX_READS = 10;

    while (total_reads < 3)
    {
        char buffer[1024];
        channel_id current_channel = 0; // Will be set by read function

        // Non-blocking read - channel_id will be modified
        ssize_t bytes_read = client->read_from_channel_blocking(current_channel,
                                                                buffer, sizeof(buffer));

        if (bytes_read > 0)
        {
            std::string message(buffer, bytes_read);
            received_messages[current_channel] = message;

            std::cout << "Read " << bytes_read << " bytes from channel "
                      << current_channel << ": " << message << std::endl;
            total_reads++;
        }
        else if (bytes_read == 0)
        {
            // No data available
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        else
        {
            // Error
            break;
        }
    }

    // Verify we got messages from correct channels
    std::cout << "\nReceived messages from " << received_messages.size() << " channels:" << std::endl;
    for (const auto &[ch, msg] : received_messages)
    {
        std::cout << "  Channel " << ch << ": " << msg << std::endl;
    }

    // Check if channel_id was properly set
    bool all_correct = true;
    for (const auto &[ch, msg] : received_messages)
    {
        if (msg.find("channel " + std::to_string(ch)) == std::string::npos)
        {
            std::cout << "❌ Channel " << ch << " message doesn't match: " << msg << std::endl;
            all_correct = false;
        }
    }

    if (all_correct)
    {
        std::cout << "✅ All channel IDs correctly set by read functions" << std::endl;
    }
}

// Test 3: Thread-safe channel reading
void test_threaded_channel_reading(std::shared_ptr<i_client> client)
{
    std::cout << "\n=== Test 3: Threaded Channel Reading ===" << std::endl;

    // Add channels
    for (int i = 10; i <= 15; i++)
    {
        client->add_channel(i, channel_type::RELIABLE_ORDERED_CHANNEL);
    }

    std::atomic<int> messages_received = 0;
    std::map<channel_id, std::atomic<int>> channel_counts;
    std::mutex cout_mutex;

    // Create multiple reader threads
    const int NUM_READERS = 3;
    std::vector<std::jthread> readers;

    for (int reader_id = 0; reader_id < NUM_READERS; reader_id++)
    {
        readers.emplace_back([&, reader_id](std::stop_token st)
                             {
            while (!st.stop_requested() && messages_received < 30) {
                char buffer[1024];
                channel_id received_channel = 0;
                
                ssize_t bytes_read = client->read_from_channel_nonblocking(
                    received_channel, buffer, sizeof(buffer));
                
                if (bytes_read > 0) {
                    messages_received++;
                    channel_counts[received_channel]++;
                    
                    {
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        std::cout << "Reader " << reader_id 
                                  << " got message from channel " << received_channel 
                                  << " (" << bytes_read << " bytes)" << std::endl;
                    }
                } else if (bytes_read == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            } });
    }

    // Sender thread
    std::jthread sender([&](std::stop_token st)
                        {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        for (int i = 0; i < 30 && !st.stop_requested(); i++) {
            channel_id target_channel = 10 + (i % 6); // Distribute among channels 10-15
            std::string msg = "Msg " + std::to_string(i) + " to ch " + 
                             std::to_string(target_channel);
            
            client->write_to_channel(target_channel, msg.c_str(), msg.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } });

    // Wait for completion
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Stop readers
    for (auto &reader : readers)
    {
        reader.request_stop();
    }

    // Print statistics
    std::cout << "\nChannel statistics:" << std::endl;
    for (int ch = 10; ch <= 15; ch++)
    {
        std::cout << "  Channel " << ch << ": " << channel_counts[ch] << " messages" << std::endl;
    }
    std::cout << "Total messages: " << messages_received << std::endl;
}

// Test 4: Blocking read with timeout and channel detection
void test_blocking_with_channel_detection(std::shared_ptr<i_client> client)
{
    std::cout << "\n=== Test 4: Blocking Read with Channel Detection ===" << std::endl;

    // Add test channels
    client->add_channel(888, channel_type::RELIABLE_ORDERED_CHANNEL);
    client->add_channel(999, channel_type::RELIABLE_ORDERED_CHANNEL);

    // Test scenario: messages arrive on different channels
    std::jthread sender([&](std::stop_token st)
                        {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // First message to channel 888
        client->write_to_channel(888, "First: Channel 888 message", 26);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Second message to channel 999
        client->write_to_channel(999, "Second: Channel 999 message", 27);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Third message back to channel 888
        client->write_to_channel(888, "Third: Another to channel 888", 29); });

    // Reader that handles multiple channels
    std::vector<std::pair<channel_id, std::string>> received;

    for (int i = 0; i < 3; i++)
    {
        char buffer[1024];
        channel_id detected_channel = 0; // Will be set by read

        std::cout << "Waiting for message " << (i + 1) << "..." << std::endl;

        ssize_t bytes_read = client->read_from_channel_blocking(detected_channel,
                                                                buffer, sizeof(buffer));

        if (bytes_read > 0)
        {
            std::string message(buffer, bytes_read);
            received.emplace_back(detected_channel, message);

            std::cout << "Received from channel " << detected_channel
                      << ": " << message << std::endl;
        }
    }

    // Verify channel detection worked
    bool correct = true;
    if (received.size() >= 1 && received[0].first != 888)
    {
        std::cout << "❌ First message should be from channel 888, got "
                  << received[0].first << std::endl;
        correct = false;
    }
    if (received.size() >= 2 && received[1].first != 999)
    {
        std::cout << "❌ Second message should be from channel 999, got "
                  << received[1].first << std::endl;
        correct = false;
    }
    if (received.size() >= 3 && received[2].first != 888)
    {
        std::cout << "❌ Third message should be from channel 888, got "
                  << received[2].first << std::endl;
        correct = false;
    }

    if (correct)
    {
        std::cout << "✅ All channel detections correct!" << std::endl;
    }
}

// Updated Test 5.3 with proper shutdown
void test_channel_ref_edge_cases(std::shared_ptr<i_client> client)
{
    std::cout << "\n=== Test 5: Edge Cases ===" << std::endl;

    // Test 5.1: Read with invalid initial channel_id
    {
        std::cout << "\nTest 5.1: Read with uninitialized channel_id" << std::endl;
        char buffer[100];
        channel_id ch_id = 0; // Always initialize!

        ssize_t result = client->read_from_channel_nonblocking(ch_id, buffer, sizeof(buffer));
        std::cout << "Result: " << result << ", channel_id after: " << ch_id << std::endl;
    }

    // Test 5.2: Multiple reads preserving channel_id
    {
        std::cout << "\nTest 5.2: Channel ID persistence across reads" << std::endl;

        client->add_channel(777, channel_type::RELIABLE_ORDERED_CHANNEL);

        // Send a message
        std::jthread sender([&]()
                            {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            client->write_to_channel(777, "Test message", 12); });

        char buffer[100];
        channel_id ch_id = 999; // Start with wrong value

        // First read should modify ch_id
        ssize_t r1 = client->read_from_channel_blocking(ch_id, buffer, sizeof(buffer));
        std::cout << "First read: result=" << r1 << ", ch_id=" << ch_id << std::endl;

        // Second read with same ch_id (now 777)
        ch_id = 888; // Reset to wrong value again
        ssize_t r2 = client->read_from_channel_nonblocking(ch_id, buffer, sizeof(buffer));
        std::cout << "Second read: result=" << r2 << ", ch_id=" << ch_id << std::endl;

        // Wait for sender thread to complete
        sender.join();
    }

    // Test 5.3: Thread-local channel_id with proper lifecycle management
    {
        std::cout << "\nTest 5.3: Thread-local channel_id" << std::endl;

        std::atomic<bool> done = false;
        std::vector<std::jthread> threads;

        // Add some channels first
        for (int i = 100; i < 105; i++)
        {
            client->add_channel(i, channel_type::RELIABLE_ORDERED_CHANNEL);
        }

        // Create reader threads
        for (int i = 0; i < 3; i++)
        {
            threads.emplace_back([&, i]()
                                 {
                channel_id local_ch_id = 0;
                char buffer[100];
                
                while (!done) {
                    ssize_t result = client->read_from_channel_nonblocking(
                        local_ch_id, buffer, sizeof(buffer));
                    
                    if (result > 0) {
                        std::cout << "Thread " << i << " read from channel " 
                                  << local_ch_id << std::endl;
                    }
                    
                    if (result < 0) {
                        // Error or client closed
                        break;
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } });
        }

        // Sender thread
        std::jthread sender([&](std::stop_token st)
                            {
            for (int i = 0; i < 5 && !st.stop_requested(); i++) {
                channel_id target_channel = 100 + (i % 5);
                std::string msg = "Thread test msg " + std::to_string(i);
                
                ssize_t sent = client->write_to_channel(target_channel, 
                                                       msg.c_str(), 
                                                       msg.size());
                if (sent > 0) {
                    std::cout << "Sent to channel " << target_channel << std::endl;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } });

        // Wait for messages to be sent
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Signal threads to stop
        done = true;
        sender.request_stop();

        // Give threads time to exit
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Threads will be automatically joined when vector goes out of scope
    }
}

int main()
{
    std::cout << "=== CHANNEL ID REFERENCE TESTS ===" << std::endl;

    try
    {
        auto client = create_client("127.0.0.1", "3003");

        // Wait for connection
        // std::this_thread::sleep_for(std::chrono::seconds(1));

        // test_channel_id_reference(client);
        // std::this_thread::sleep_for(std::chrono::seconds(1));

        // test_multiple_channels_with_refs(client);
        // std::this_thread::sleep_for(std::chrono::seconds(1));

        // test_threaded_channel_reading(client);
        // std::this_thread::sleep_for(std::chrono::seconds(1));

        // test_blocking_with_channel_detection(client);
        // std::this_thread::sleep_for(std::chrono::seconds(1));

        test_channel_ref_edge_cases(client);
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Clean shutdown
        std::cout << "\nClosing client..." << std::endl;
        client->close_client();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n=== All tests completed ===" << std::endl;
    return 0;
}