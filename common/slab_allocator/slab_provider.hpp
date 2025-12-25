#pragma once
#include <memory>
#include <utility>
#include "cache.hpp"

// Tag-based provider for Typed Objects
template <typename T, string_literal Tag>
class slab_provider
{
public:
    /**
     * @brief Allocates memory from the slab and constructs an object of type T.
     * @returns A unique_ptr with a custom deleter that calls the destructor and returns memory to the slab.
     */
    template <typename... Args>
    static std::unique_ptr<T, void (*)(T *)> get_unique(Args &&...args)
    {
        auto &cache = cache_t<sizeof(T), Tag>::get_instance();

        void *raw = cache.thread_safe_alloc();
        if (__builtin_expect(raw == nullptr, 0))
            return nullptr;

        T *obj = new (raw) T(std::forward<Args>(args)...);

        return std::unique_ptr<T, void (*)(T *)>(obj, [](T *p)
                                                 {
            if (p) {
                p->~T();
                cache_t<sizeof(T), Tag>::get_instance().thread_safe_free(p);
            } });
    }

    static T *alloc_raw()
    {
        return static_cast<T *>(cache_t<sizeof(T), Tag>::get_instance().thread_safe_alloc());
    }

    static void free_raw(T *to_free)
    {
        cache_t<sizeof(T), Tag>::get_instance().thread_safe_free(to_free);
    }
};

// Tag-based provider for Raw Memory Buffers (e.g., char arrays for RUDP packets)
template <size_t ObjSize, string_literal Tag>
class slab_memory
{
public:
    static cache_t<ObjSize, Tag> &get_cache()
    {
        return cache_t<ObjSize, Tag>::get_instance();
    }

    static void *alloc()
    {
        return get_cache().thread_safe_alloc();
    }

    static void free(void *ptr)
    {
        get_cache().thread_safe_free(ptr);
    }
};