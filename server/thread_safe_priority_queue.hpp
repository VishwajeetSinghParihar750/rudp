#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <chrono>

template <typename T, typename Container = std::vector<T>, typename Compare = std::less<T>>
class thread_safe_priority_queue
{
public:
    void push(T &&value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.push(std::move(value));
        cv_.notify_one();
    }

    template <typename... Args>
    void emplace(Args &&...args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.emplace(std::forward<Args>(args)...);
        cv_.notify_one();
    }

    bool pop(T &value)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (data_.empty())
        {
            return false;
        }

        value = std::move(data_.top());
        data_.pop();
        return true;
    }

    void wait_and_pop(T &value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });
        value = std::move(data_.top());
        data_.pop();
    }

    T wait_and_pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });
        T value = std::move(data_.top());
        data_.pop();
        return value;
    }

    bool top(T &value) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (data_.empty())
        {
            return false;
        }

        value = data_.top();
        return true;
    }

    void wait_and_top(T &value) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });
        value = data_.top();
    }

    T wait_and_top() const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_.empty(); });
        return data_.top();
    }

    bool wait_for_and_pop(T &value, const std::chrono::milliseconds &timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        bool status = cv_.wait_for(lock, timeout, [this]
                                   { return !data_.empty(); });

        if (status && !data_.empty())
        {
            value = data_.top();
            data_.pop();
            return true;
        }
        return false;
    }

    bool wait_for_and_top(T &value, const std::chrono::milliseconds &timeout) const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        bool status = cv_.wait_for(lock, timeout, [this]
                                   { return !data_.empty(); });

        if (status)
        {
            value = data_.top();
            return true;
        }
        return false;
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
    std::priority_queue<
        T,
        Container,
        Compare>
        data_;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
};