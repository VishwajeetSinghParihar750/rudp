#pragma once
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cassert>
#include <new>
#include <iostream>
#include <thread>
#include "slab.hpp"

namespace
{
    static constexpr uint32_t MIN_OBJECTS_PER_SLAB = 8;
    static constexpr uint32_t MIN_OBJECT_SIZE = 16;
    // TARGET_CHUNK_SIZE: We aim to mmap ~2MB at a time.
    // If Slab is 4KB, we grab 512 slabs.
    // If Slab is 256KB, we grab 8 slabs.
    static constexpr size_t TARGET_CHUNK_SIZE = 2 * 1024 * 1024;
    static const uint32_t CACHE_LINE_SIZE = sysconf(_SC_LEVEL1_DCACHE_LINESIZE) > 0 ? sysconf(_SC_LEVEL1_DCACHE_LINESIZE) : 64;
    static constexpr uint32_t MAX_LOCAL_EMPTY_SLABS = 32;

    template <typename T>
    constexpr T align_up(T n, size_t align = alignof(std::max_align_t)) noexcept
    {
        return (n + align - 1) & ~(align - 1);
    }
}

struct thread_context
{
    slab_t *active = nullptr;
    slab_t list_partial;
    slab_t list_full;
    slab_t list_empty;
    uint32_t empty_slab_count = 0;
    uint32_t scavenge_cooldown = 0;

    thread_context()
    {
    }
};

template <size_t N>
struct string_literal
{
    char value[N];
    constexpr string_literal(const char (&str)[N])
    {
        std::copy_n(str, N, value);
    }
};

template <size_t object_size, string_literal tag>
class cache_t
{
    using ctor = void (*)(void *);
    using dtor = void (*)(void *);

    uint32_t obj_size, obj_cnt;
    uint64_t PAGE_SIZE;
    uint32_t pages_per_chunk; // DYNAMIC CHUNK SIZE

    uint32_t color, color_offset;
    std::atomic<uint32_t> color_next;

    slab_t global_empty;
    std::mutex global_mtx;

    std::vector<std::pair<void *, size_t>> mapped_pages;

    ctor cons;
    dtor dest;

    static inline thread_local thread_context *my_ctx = nullptr;

public:
    static cache_t &get_instance()
    {
        static cache_t instance{};
        return instance;
    }

    ~cache_t()
    {
        std::lock_guard<std::mutex> lock(global_mtx);
        for (auto &[ptr, size] : mapped_pages)
        {
            munmap(ptr, size);
        }
        mapped_pages.clear();
    }

    void *thread_safe_alloc()
    {
        if (__builtin_expect(my_ctx == nullptr, 0))
            init_thread();

        slab_t *s = nullptr;

        s = my_ctx->active;
        if (s)
        {
            if (s->local_head)
                return pop_local(s);

            s->unlink();
            s->link_after(&my_ctx->list_full);
            my_ctx->active = nullptr;
        }

        if (!my_ctx->list_empty.is_empty_list())
        {
            s = my_ctx->list_empty.next;
            s->unlink();
            my_ctx->empty_slab_count--;
            my_ctx->active = s;
            return pop_local(s);
        }

        while (!my_ctx->list_partial.is_empty_list())
        {
            s = my_ctx->list_partial.next;

            if (s->local_head || reclaim_remote(s))
            {
                s->unlink();
                my_ctx->active = s;
                return pop_local(s);
            }

            // If it somehow became full/empty, move it
            s->link_after(&my_ctx->list_full);
        }

        if (my_ctx->scavenge_cooldown > 0)
        {
            my_ctx->scavenge_cooldown--;
        }
        else if (!my_ctx->list_full.is_empty_list())
        {
            int attempts = 64;
            slab_t *curr = my_ctx->list_full.prev;
            bool found = false;

            while (attempts-- > 0 && curr != &my_ctx->list_full)
            {
                slab_t *prev_node = curr->prev; // Save prev before potentially modifying

                if (curr->atomic_head.load(std::memory_order_relaxed) != nullptr)
                {
                    if (reclaim_remote(curr))
                    {
                        curr->unlink();
                        my_ctx->active = curr;

                        my_ctx->scavenge_cooldown = 0;

                        return pop_local(curr);
                    }
                }

                curr = prev_node;
            }

            my_ctx->scavenge_cooldown = 64;
        }

        s = fetch_global_slab();
        my_ctx->active = s;
        if (s->next != s)
            s->unlink();
        return pop_local(s);
    }

    void thread_safe_free(void *obj)
    {
        if (__builtin_expect(my_ctx == nullptr, 0))
            init_thread();

        uintptr_t addr = (uintptr_t)obj;
        slab_t *slab = (slab_t *)(addr & ~(PAGE_SIZE - 1));

        if (dest)
            dest(obj);

        if (__builtin_expect(slab->owner == my_ctx, 1))
        {
            *((void **)obj) = slab->local_head;
            slab->local_head = obj;
            uint32_t active = --slab->active_obj_cnt;

            if (slab == my_ctx->active)
                return;

            if (active == obj_cnt - 1)
            {
                slab->unlink();
                slab->link_after(&my_ctx->list_partial);
            }
            else if (active == 0)
            {
                slab->unlink();
                slab->link_after(&my_ctx->list_empty);
                my_ctx->empty_slab_count++;
                if (my_ctx->empty_slab_count > MAX_LOCAL_EMPTY_SLABS)
                {
                    return_slabs_to_global();
                }
            }
        }
        else
        {
            void *old_head = slab->atomic_head.load(std::memory_order_relaxed);
            do
            {
                *((void **)obj) = old_head;
            } while (!slab->atomic_head.compare_exchange_weak(old_head, obj, std::memory_order_release, std::memory_order_relaxed));
        }
    }

private:
    cache_t(ctor ctr = nullptr, dtor dtr = nullptr)
        : obj_size(object_size), cons(ctr), dest(dtr)
    {
        global_empty.next = global_empty.prev = &global_empty;

        assert(obj_size > 0);

        obj_size = std::max(static_cast<uint32_t>(object_size), MIN_OBJECT_SIZE);
        if (obj_size < sizeof(void *))
            obj_size = sizeof(void *);

        obj_size = (1 << (32 - __builtin_clz(obj_size - 1)));

        uint32_t metadata_req = align_up(sizeof(slab_t), CACHE_LINE_SIZE);
        uint32_t required = obj_size * MIN_OBJECTS_PER_SLAB + metadata_req;

        PAGE_SIZE = (1 << (32 - __builtin_clz(required - 1)));
        if (PAGE_SIZE < 4096)
            PAGE_SIZE = 4096;

        pages_per_chunk = TARGET_CHUNK_SIZE / PAGE_SIZE;
        if (pages_per_chunk == 0)
            pages_per_chunk = 1;

        obj_cnt = (PAGE_SIZE - metadata_req) / obj_size;
        uint32_t total_used = metadata_req + (obj_cnt * obj_size);
        uint32_t size_left = PAGE_SIZE - total_used;

        color = (size_left / CACHE_LINE_SIZE) + 1;
        color_next = 0;
        color_offset = CACHE_LINE_SIZE;
    }

    void init_thread()
    {
        my_ctx = new thread_context();
    }

    inline void *pop_local(slab_t *s)
    {
        void *obj = s->local_head;
        s->local_head = *((void **)obj);
        s->active_obj_cnt++;
        if (cons)
            cons(obj);
        return obj;
    }

    uint32_t reclaim_remote(slab_t *s)
    {
        void *peek = s->atomic_head.load(std::memory_order_relaxed);
        if (peek == nullptr)
            return 0;

        void *remote = s->atomic_head.exchange(nullptr, std::memory_order_acquire);

        if (!remote)
            return 0;

        uint32_t count = 0;
        void *cur = remote;
        void *last = nullptr;

        while (cur)
        {
            count++;
            last = cur;
            cur = *((void **)cur);
        }

        *((void **)last) = s->local_head;

        s->local_head = remote;
        s->active_obj_cnt -= count;

        return count;
    }

    slab_t *fetch_global_slab()
    {
        std::lock_guard<std::mutex> lock(global_mtx);
        if (global_empty.is_empty_list())
        {
            allocate_free_slab();
        }
        slab_t *fresh = global_empty.next;
        fresh->unlink();
        fresh->owner = my_ctx;
        return fresh;
    }

    void return_slabs_to_global()
    {
        int to_move = my_ctx->empty_slab_count / 2;
        std::lock_guard<std::mutex> lock(global_mtx);
        while (to_move > 0 && !my_ctx->list_empty.is_empty_list())
        {
            slab_t *s = my_ctx->list_empty.next;
            s->unlink();
            s->owner = nullptr;
            s->link_after(&global_empty);
            my_ctx->empty_slab_count--;
            to_move--;
        }
    }

    void initialize_slab(void *mem, bool is_aligned, bool is_front)
    {
        slab_t *slab_obj = new (mem) slab_t(is_aligned, is_front);
        uint8_t *memptr = reinterpret_cast<uint8_t *>(mem);
        uint32_t current_color_idx = color_next.fetch_add(1, std::memory_order_relaxed) % color;

        uintptr_t metadata_end = align_up((uintptr_t)memptr + sizeof(slab_t), CACHE_LINE_SIZE);
        slab_obj->mem = (void *)(metadata_end + (current_color_idx * color_offset));
        uint8_t *start = (uint8_t *)slab_obj->mem;
        for (uint32_t i = 0; i < obj_cnt - 1; i++)
        {
            *((void **)(start + (i * obj_size))) = start + ((i + 1) * obj_size);
        }
        *((void **)(start + (obj_cnt - 1) * obj_size)) = nullptr;

        slab_obj->local_head = start;
        slab_obj->active_obj_cnt = 0;
        slab_obj->owner = nullptr;
        slab_obj->atomic_head.store(nullptr, std::memory_order_relaxed);

        slab_obj->link_after(&global_empty);
    }

    void allocate_free_slab()
    {
        size_t alloc_size = PAGE_SIZE * pages_per_chunk;
        void *pos_mem = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(pos_mem != MAP_FAILED);

        static uint32_t total_alloc = 0;
        total_alloc += alloc_size;
        if (total_alloc > 1ull * 1024 * 1024 * 1024)
        {
            std::cerr << "hard limit of 1gb crossed " << std::endl;
            std::abort();
        }

        mapped_pages.emplace_back(pos_mem, alloc_size);

        uintptr_t mem_location = reinterpret_cast<uintptr_t>(pos_mem);
        uintptr_t og_mem_location = mem_location;

        if (mem_location % PAGE_SIZE)
        {
            mem_location = (mem_location + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
            uintptr_t *metadata_space = (uintptr_t *)mem_location;
            metadata_space[-1] = og_mem_location;
        }

        void *mem = reinterpret_cast<void *>(mem_location);

        bool got_aligned = (mem_location == og_mem_location);
        for (uint32_t i = 0; i < pages_per_chunk - (!got_aligned); i++)
        {
            initialize_slab((uint8_t *)mem + PAGE_SIZE * i, got_aligned, i == 0);
        }
    }
};
