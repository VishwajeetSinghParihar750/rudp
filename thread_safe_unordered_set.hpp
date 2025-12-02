#pragma once

#include <unordered_set>
#include <mutex>
#include <condition_variable>

template <typename T>
class thread_safe_unordered_set
{
private:
    std::unordered_set<T> set_;
    mutable std::mutex mutex_;
    std::condition_variable cv;

public:
    thread_safe_unordered_set() = default;

    thread_safe_unordered_set(const thread_safe_unordered_set &) = delete;
    thread_safe_unordered_set &operator=(const thread_safe_unordered_set &) = delete;

    bool insert(const T &value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto [it, inserted] = set_.insert(value);

        if (inserted)
        {
            cv.notify_one();
        }
        return inserted;
    }

    bool erase(const T &value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return set_.erase(value) > 0;
    }

    std::pair<bool, T> try_pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (set_.empty())
            return {false, T{}};

        T toret = *set_.begin();
        set_.erase(set_.begin());
        return {true, toret};
    }

    T pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        cv.wait(lock, [&]
                { return !set_.empty(); });

        T toret = *set_.begin();
        set_.erase(set_.begin());
        return toret;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_.size();
    }

    bool contains(const T &value) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_.count(value) > 0;
    }
};