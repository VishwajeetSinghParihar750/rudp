#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>

template <typename T,
          typename Container = std::vector<T>,
          typename Compare = std::less<T>>
class thread_safe_priority_queue
{
public:
    // WRITE
    void push(T &&value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            data_.push(std::move(value));
        }
        cv_.notify_one();
    }

    template <typename... Args>
    void emplace(Args &&...args)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            data_.emplace(std::forward<Args>(args)...);
        }
        cv_.notify_one();
    }

    // WRITE
    bool pop(T &value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (data_.empty())
            return false;

        value = data_.top(); // copy (shared_ptr copy is cheap)
        data_.pop();
        return true;
    }

    // WRITE (blocking)
    void wait_and_pop(T &value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });

        value = data_.top();
        data_.pop();
    }

    // WRITE (blocking)
    T wait_and_pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });

        T value = data_.top();
        data_.pop();
        return value;
    }

    // READ
    bool top(T &value) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (data_.empty())
            return false;

        value = data_.top();
        return true;
    }

    // READ (blocking)
    void wait_and_top(T &value) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });

        value = data_.top();
    }

    // READ (blocking)
    T wait_and_top() const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });

        return data_.top();
    }

    // WRITE (timed)
    bool wait_for_and_pop(T &value,
                          const std::chrono::milliseconds &timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout,
                          [this]
                          { return !data_.empty(); }))
            return false;

        value = data_.top();
        data_.pop();
        return true;
    }

    // READ (timed)
    bool wait_for_and_top(T &value,
                          const std::chrono::milliseconds &timeout) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout,
                          [this]
                          { return !data_.empty(); }))
            return false;

        value = data_.top();
        return true;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.empty();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.size();
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::priority_queue<T, Container, Compare> data_;
};
