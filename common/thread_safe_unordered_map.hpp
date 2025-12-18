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

template <typename K, typename V, typename H = std::hash<K>>
class thread_safe_unordered_map
{
private:
    struct Bucket
    {
        std::unordered_map<K, V, H> map;
        mutable std::shared_mutex mutex; // protects map
        std::mutex cv_mutex;             // ONLY for condition_variable
        std::condition_variable cv;
    };

    static constexpr size_t num_shards = 8;
    std::vector<std::unique_ptr<Bucket>> shards_;
    H hasher_;

    Bucket &get_bucket(const K &key) const
    {
        return *shards_[hasher_(key) % num_shards];
    }

public:
    thread_safe_unordered_map()
    {
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
            auto [_, inserted] =
                bucket.map.insert_or_assign(key, std::move(value));
            if (!inserted)
                return false;
        }
        bucket.cv.notify_all();
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
        for (size_t i = 0; i < num_shards; ++i)
        {
            auto &bucket = *shards_[i];
            std::unique_lock<std::shared_mutex> lock(bucket.mutex, std::try_to_lock);
            if (lock.owns_lock() && !bucket.map.empty())
            {
                auto it = bucket.map.begin();
                std::pair<K, V> result(it->first, std::move(it->second));
                bucket.map.erase(it);
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
            for (size_t i = 0; i < num_shards; ++i)
            {
                auto &bucket = *shards_[i];
                std::unique_lock<std::shared_mutex> lock(bucket.mutex, std::try_to_lock);
                if (lock.owns_lock() && !bucket.map.empty())
                {
                    auto it = bucket.map.begin();
                    std::pair<K, V> result(it->first, std::move(it->second));
                    bucket.map.erase(it);
                    return result;
                }
            }

            // sleep
            auto &b = *shards_[0];
            std::unique_lock<std::mutex> cv_lock(b.cv_mutex);
            b.cv.wait_for(cv_lock, std::chrono::milliseconds(1));
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
