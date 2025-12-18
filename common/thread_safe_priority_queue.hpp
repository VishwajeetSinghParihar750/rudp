#pragma once

#include <queue>
#include <shared_mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <chrono>

template <typename T, typename Container = std::vector<T>, typename Compare = std::less<T>>
class thread_safe_priority_queue
{
public:
    // WRITE
    void push(T &&value)
    {
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            data_.push(std::move(value));
        }
        cv_.notify_one();
    }

    template <typename... Args>
    void emplace(Args &&...args)
    {
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            data_.emplace(std::forward<Args>(args)...);
        }
        cv_.notify_one();
    }

    // WRITE
    bool pop(T &value)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (data_.empty())
            return false;

        value = std::move(const_cast<T &>(data_.top()));
        data_.pop();
        return true;
    }

    // WRITE
    void wait_and_pop(T &value)
    {
        wait_for_data();
        std::unique_lock<std::shared_mutex> lock(mutex_);
        value = std::move(const_cast<T &>(data_.top()));
        data_.pop();
    }

    // WRITE
    T wait_and_pop()
    {
        wait_for_data();
        std::unique_lock<std::shared_mutex> lock(mutex_);
        T value = std::move(const_cast<T &>(data_.top()));
        data_.pop();
        return value;
    }

    // READ
    bool top(T &value) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (data_.empty())
            return false;

        value = data_.top();
        return true;
    }

    // READ (blocking)
    void wait_and_top(T &value) const
    {
        wait_for_data();
        std::shared_lock<std::shared_mutex> lock(mutex_);
        value = data_.top();
    }

    // READ (blocking)
    T wait_and_top() const
    {
        wait_for_data();
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.top();
    }

    // WRITE (timed)
    bool wait_for_and_pop(T &value, const std::chrono::milliseconds &timeout)
    {
        if (!wait_for_data(timeout))
            return false;

        std::unique_lock<std::shared_mutex> lock(mutex_);
        value = std::move(const_cast<T &>(data_.top()));
        data_.pop();
        return true;
    }

    // READ (timed)
    bool wait_for_and_top(T &value, const std::chrono::milliseconds &timeout) const
    {
        if (!wait_for_data(timeout))
            return false;

        std::shared_lock<std::shared_mutex> lock(mutex_);
        value = data_.top();
        return true;
    }

    bool empty() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.empty();
    }

    size_t size() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return data_.size();
    }

private:
    // Wait helpers (NO shared_mutex involved)
    void wait_for_data() const
    {
        std::unique_lock<std::mutex> lock(cv_mtx_);
        cv_.wait(lock, [this]
                 {
            std::shared_lock<std::shared_mutex> data_lock(mutex_);
            return !data_.empty(); });
    }

    bool wait_for_data(const std::chrono::milliseconds &timeout) const
    {
        std::unique_lock<std::mutex> lock(cv_mtx_);
        return cv_.wait_for(lock, timeout, [this]
                            {
            std::shared_lock<std::shared_mutex> data_lock(mutex_);
            return !data_.empty(); });
    }

private:
    std::priority_queue<T, Container, Compare> data_;
    mutable std::shared_mutex mutex_; // protects data_

    // CV infrastructure
    mutable std::mutex cv_mtx_;
    mutable std::condition_variable cv_;
};
