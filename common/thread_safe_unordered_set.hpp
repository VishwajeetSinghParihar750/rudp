#pragma once

#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <optional>
#include <vector>
#include <memory>
#include <algorithm>
#include <chrono>

template <typename T, typename H = std::hash<T>>
class thread_safe_unordered_set
{
private:
    struct Bucket
    {
        std::unordered_set<T> set;
        mutable std::shared_mutex mutex;
    };

    static constexpr size_t num_shards = 16;
    std::vector<std::unique_ptr<Bucket>> shards_;

    std::condition_variable cv_; // standard CV
    std::mutex cv_mtx_;          // mutex ONLY for cv
    H hasher_;

    Bucket &get_bucket(const T &value) const
    {
        return *shards_[hasher_(value) % num_shards];
    }

public:
    thread_safe_unordered_set()
    {
        for (size_t i = 0; i < num_shards; ++i)
            shards_.emplace_back(std::make_unique<Bucket>());
    }

    thread_safe_unordered_set(const thread_safe_unordered_set &) = delete;
    thread_safe_unordered_set &operator=(const thread_safe_unordered_set &) = delete;

    // WRITE
    bool insert(const T &value)
    {
        auto &bucket = get_bucket(value);
        {
            std::unique_lock<std::shared_mutex> lock(bucket.mutex);
            auto [_, inserted] = bucket.set.insert(value);
            if (!inserted)
                return false;
        }
        cv_.notify_all();
        return true;
    }

    // WRITE
    bool erase(const T &value)
    {
        auto &bucket = get_bucket(value);
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        return bucket.set.erase(value) > 0;
    }

    // READ
    bool contains(const T &value) const
    {
        auto &bucket = get_bucket(value);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex);
        return bucket.set.count(value) > 0;
    }

    // WRITE (non-blocking)
    std::optional<T> try_pop()
    {
        for (size_t i = 0; i < num_shards; ++i)
        {
            auto &bucket = *shards_[i];
            std::unique_lock<std::shared_mutex> lock(bucket.mutex, std::try_to_lock);
            if (lock.owns_lock() && !bucket.set.empty())
            {
                auto it = bucket.set.begin();
                T val = *it;
                bucket.set.erase(it);
                return val;
            }
        }
        return std::nullopt;
    }

    // WRITE (blocking)
    T pop()
    {
        while (true)
        {
            for (size_t i = 0; i < num_shards; ++i)
            {
                auto &bucket = *shards_[i];
                std::unique_lock<std::shared_mutex> lock(bucket.mutex, std::try_to_lock);
                if (lock.owns_lock() && !bucket.set.empty())
                {
                    auto it = bucket.set.begin();
                    T val = *it;
                    bucket.set.erase(it);
                    return val;
                }
            }

            // sleep
            std::unique_lock<std::mutex> cv_lock(cv_mtx_);
            cv_.wait_for(cv_lock, std::chrono::milliseconds(10));
        }
    }

    // READ
    size_t size() const
    {
        size_t total = 0;
        for (const auto &b : shards_)
        {
            std::shared_lock<std::shared_mutex> lock(b->mutex);
            total += b->set.size();
        }
        return total;
    }

    // READ (expensive but safe)
    std::unordered_set<T> copy_all() const
    {
        std::unordered_set<T> consolidated;
        for (const auto &b : shards_)
        {
            std::shared_lock<std::shared_mutex> lock(b->mutex);
            consolidated.insert(b->set.begin(), b->set.end());
        }
        return consolidated;
    }
};
