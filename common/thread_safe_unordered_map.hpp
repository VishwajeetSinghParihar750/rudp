#pragma once

#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <optional>
#include <utility>
#include <vector>
#include <memory>
#include <chrono>
#include <atomic>

template <typename K, typename V, typename H = std::hash<K>>
class thread_safe_unordered_map
{
private:
    struct Bucket
    {
        std::unordered_map<K, V, H> map;
        mutable std::shared_mutex mutex;
    };

    static constexpr size_t num_shards = 8;
    std::vector<std::unique_ptr<Bucket>> shards_;
    H hasher_;

    // GLOBAL CV (required for correctness)
    mutable std::mutex cv_mutex_;
    mutable std::condition_variable cv_;

    Bucket &get_bucket(const K &key) const
    {
        return *shards_[hasher_(key) % num_shards];
    }

    bool has_any_data() const
    {
        for (const auto &b : shards_)
        {
            std::shared_lock<std::shared_mutex> lock(b->mutex);
            if (!b->map.empty())
                return true;
        }
        return false;
    }

public:
    thread_safe_unordered_map()
    {
        shards_.reserve(num_shards);
        for (size_t i = 0; i < num_shards; ++i)
            shards_.emplace_back(std::make_unique<Bucket>());
    }

    thread_safe_unordered_map(const thread_safe_unordered_map &) = delete;
    thread_safe_unordered_map &operator=(const thread_safe_unordered_map &) = delete;

    // WRITE
    bool insert(const K &key, V value)
    {
        auto &bucket = get_bucket(key);
        {
            std::unique_lock<std::shared_mutex> lock(bucket.mutex);
            bucket.map.insert_or_assign(key, std::move(value));
        }
        cv_.notify_one();
        return true;
    }

    // WRITE
    bool erase(const K &key)
    {
        auto &bucket = get_bucket(key);
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        return bucket.map.erase(key) > 0;
    }

    // READ
    bool contains(const K &key) const
    {
        auto &bucket = get_bucket(key);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex);
        return bucket.map.find(key) != bucket.map.end();
    }

    // READ
    std::optional<V> get(const K &key) const
    {
        auto &bucket = get_bucket(key);
        std::shared_lock<std::shared_mutex> lock(bucket.mutex);
        auto it = bucket.map.find(key);
        if (it == bucket.map.end())
            return std::nullopt;
        return it->second;
    }

    // WRITE (non-blocking)
    std::optional<std::pair<K, V>> try_pop()
    {
        for (auto &b : shards_)
        {
            std::unique_lock<std::shared_mutex> lock(b->mutex, std::try_to_lock);
            if (lock.owns_lock() && !b->map.empty())
            {
                auto it = b->map.begin();
                std::pair<K, V> result(it->first, std::move(it->second));
                b->map.erase(it);
                return result;
            }
        }
        return std::nullopt;
    }

    // WRITE (blocking)
    std::pair<K, V> pop()
    {
        while (true)
        {
            // Fast path
            if (auto v = try_pop())
                return std::move(*v);

            // Proper blocking wait
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
            total += b->map.size();
        }
        return total;
    }

    // WRITE
    void clear()
    {
        for (auto &b : shards_)
        {
            std::unique_lock<std::shared_mutex> lock(b->mutex);
            b->map.clear();
        }
    }

    // READ
    std::vector<std::pair<K, V>> items() const
    {
        std::vector<std::pair<K, V>> res;
        for (const auto &b : shards_)
        {
            std::shared_lock<std::shared_mutex> lock(b->mutex);
            res.reserve(res.size() + b->map.size());
            for (const auto &kv : b->map)
                res.emplace_back(kv.first, kv.second);
        }
        return res;
    }
};
