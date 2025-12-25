#pragma once

#include <string>
#include <iostream>
#include <ostream>
#include <mutex>
#include <sstream>

class logger
{
    bool info_on = false;
    bool error_on = true;
    bool warning_on = true;
    bool test_on = false;
    bool critical_on = false;
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

    bool isInfoEnabled() const { return info_on; }
    bool isWarningEnabled() const { return warning_on; }
    bool isErrorEnabled() const { return error_on; }
    bool isCriticalEnabled() const { return critical_on; }
    bool isTestEnabled() const { return test_on; }

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

#define LOG_INFO(msg)                                 \
    do                                                \
    {                                                 \
        if (logger::getInstance().isInfoEnabled())    \
        {                                             \
            std::ostringstream oss;                   \
            oss << msg;                               \
            logger::getInstance().logInfo(oss.str()); \
        }                                             \
    } while (0)

#define LOG_WARN(msg)                                    \
    do                                                   \
    {                                                    \
        if (logger::getInstance().isWarningEnabled())    \
        {                                                \
            std::ostringstream oss;                      \
            oss << msg;                                  \
            logger::getInstance().logWarning(oss.str()); \
        }                                                \
    } while (0)

#define LOG_ERROR(msg)                                 \
    do                                                 \
    {                                                  \
        if (logger::getInstance().isErrorEnabled())    \
        {                                              \
            std::ostringstream oss;                    \
            oss << msg;                                \
            logger::getInstance().logError(oss.str()); \
        }                                              \
    } while (0)

#define LOG_CRITICAL(msg)                                 \
    do                                                    \
    {                                                     \
        if (logger::getInstance().isCriticalEnabled())    \
        {                                                 \
            std::ostringstream oss;                       \
            oss << msg;                                   \
            logger::getInstance().logCritical(oss.str()); \
        }                                                 \
    } while (0)

#define LOG_TEST(msg)                                 \
    do                                                \
    {                                                 \
        if (logger::getInstance().isTestEnabled())    \
        {                                             \
            std::ostringstream oss;                   \
            oss << msg;                               \
            logger::getInstance().logTest(oss.str()); \
        }                                             \
    } while (0)
