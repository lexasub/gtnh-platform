#pragma once

#include "ClockCache.h"
#include "../../Chunk/Chunk.h"
#include <cstdint>
#include <functional>

// Lock-free chunk cache. Owns Chunk* — deletes on eviction.
class ChunkCache {
public:
    ChunkCache() noexcept {
        cache_.set_on_evict([](uintptr_t val) {
            delete reinterpret_cast<Chunk*>(val);
        });
    }

    const Chunk* get(int64_t key) const noexcept {
        auto opt = cache_.get(key);
        return opt ? reinterpret_cast<const Chunk*>(*opt) : nullptr;
    }

    void put(int64_t key, Chunk* chunk) noexcept {
        cache_.put(key, reinterpret_cast<uintptr_t>(chunk));
    }

    void erase(int64_t key) noexcept {
        cache_.erase(key);
    }

    void clear() noexcept {
        cache_.clear();
    }

private:
    mutable ClockCache<uintptr_t, 1024> cache_;
};
