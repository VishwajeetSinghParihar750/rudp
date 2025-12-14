#pragma once

#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <utility>

template <typename K, typename V, typename H = std::hash<K>>
class thread_safe_unordered_map
{
private:
    std::unordered_map<K, V, H> map_;
    mutable std::mutex mutex_;
    std::condition_variable cv;

public:
    thread_safe_unordered_map() = default;

    thread_safe_unordered_map(const thread_safe_unordered_map &) = delete;
    thread_safe_unordered_map &operator=(const thread_safe_unordered_map &) = delete;

    bool insert(const K &key, V value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto [it, inserted] = map_.insert_or_assign(key, std::move(value));
        if (inserted)
        {
            cv.notify_one();
        }
        return inserted;
    }

    bool erase(const K &key)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return map_.erase(key) > 0;
    }

    bool contains(const K &key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.find(key) != map_.end();
    }

    std::optional<V> get(const K &key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<std::pair<K, V>> try_pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (map_.empty())
            return std::nullopt;

        auto it = map_.begin();
        std::pair<K, V> result = std::make_pair(it->first, std::move(it->second));
        map_.erase(it);
        return result;
    }

    std::pair<K, V> pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv.wait(lock, [this]
                { return !map_.empty(); });

        auto it = map_.begin();
        std::pair<K, V> result = std::make_pair(it->first, std::move(it->second));
        map_.erase(it);
        return result;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
    }
};