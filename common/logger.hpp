#pragma once

#include <string>
#include <iostream>
#include <ostream>
#include <mutex>

class logger
{
    bool info_on = false;
    bool error_on = true; // Error should typically be ON by default
    bool test_on = false;
    std::mutex log_mutex;

    logger() = default;
    ~logger() = default;

public:
    logger(const logger &) = delete;
    logger &operator=(const logger &) = delete;
    logger(logger &&) = delete;
    logger &operator=(logger &&) = delete;

    static logger &getInstance()
    {
        static logger instance;
        return instance;
    }

    void setInfoEnabled(bool enable) { info_on = enable; }
    void setErrorEnabled(bool enable) { error_on = enable; }
    void setTestEnabled(bool enable) { test_on = enable; }

    void logInfo(const std::string &msg)
    {
        if (info_on) {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "[INFO] " << msg << '\n';
        }
    }

    void logError(const std::string &msg)
    {
        if (error_on) {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cerr << "[ERROR] " << msg << '\n';
        }
    }

    void testLog(const std::string &msg)
    {
        if (test_on) {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "[TEST] " << msg << '\n';
        }
    }
};