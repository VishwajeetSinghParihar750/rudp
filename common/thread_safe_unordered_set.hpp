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
    H hasher_;

    // GLOBAL CV (required for correctness)
    mutable std::mutex cv_mutex_;
    mutable std::condition_variable cv_;

    Bucket &get_bucket(const T &value) const
    {
        return *shards_[hasher_(value) % num_shards];
    }

    bool has_any_data() const
    {
        for (const auto &b : shards_)
        {
            std::shared_lock<std::shared_mutex> lock(b->mutex);
            if (!b->set.empty())
                return true;
        }
        return false;
    }

public:
    thread_safe_unordered_set()
    {
        shards_.reserve(num_shards);
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
        cv_.notify_one();
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
        for (auto &b : shards_)
        {
            std::unique_lock<std::shared_mutex> lock(b->mutex, std::try_to_lock);
            if (lock.owns_lock() && !b->set.empty())
            {
                auto it = b->set.begin();
                T val = *it;
                b->set.erase(it);
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
            if (auto v = try_pop())
                return std::move(*v);

            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait(lk, [this]
                     { return has_any_data(); });
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
