#pragma once

#include <string>
#include <iostream>
#include <ostream>
#include <mutex>

class logger
{
    bool info_on = true;
    bool error_on = true;
    bool warning_on = true;
    bool test_on = true;
    bool critical_on = true;
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
    void setWarningEnabled(bool enable) { warning_on = enable; }
    void setTestEnabled(bool enable) { test_on = enable; }
    void setCriticalEnabled(bool enable) { critical_on = enable; }

    void logInfo(const std::string &msg)
    {
        if (info_on)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "✨ [INFO] " << msg << '\n';
        }
    }

    void logWarning(const std::string &msg)
    {
        if (warning_on)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cerr << "⚠️ [WARN] " << msg << '\n';
        }
    }

    void logError(const std::string &msg)
    {
        if (error_on)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cerr << "❌ [ERROR] " << msg << '\n';
        }
    }

    void logCritical(const std::string &msg)
    {
        if (critical_on)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cerr << "🚨 [CRITICAL] " << msg << '\n';
        }
    }

    void logTest(const std::string &msg)
    {
        if (test_on)
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            std::cout << "🧪 [TEST] " << msg << '\n';
        }
    }
};