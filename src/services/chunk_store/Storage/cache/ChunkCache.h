#pragma once

#include "ClockCache.h"
#include "../../Chunk/Chunk.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChunkCache {
public:
    static constexpr size_t POOL_CAP = 256;

    ChunkCache() noexcept {
        cache_.set_on_evict([this](uintptr_t val) {
            if (!val) return;
            auto* chunk = reinterpret_cast<Chunk*>(val);
            std::lock_guard plk(pin_mutex_);
            if (pinned_.contains(chunk)) {
                pending_evict_.insert(chunk);
                return;
            }
            std::lock_guard lock(pool_mutex_);
            if (pool_.size() < POOL_CAP) {
                pool_.push_back(chunk);
            } else {
                delete chunk;
            }
        });
    }

    const Chunk* get(int64_t key) const noexcept {
        auto opt = cache_.get(key);
        return opt ? reinterpret_cast<const Chunk*>(*opt) : nullptr;
    }

    const Chunk* getPinned(int64_t key) noexcept {
        std::lock_guard lock(pin_mutex_);
        auto opt = cache_.get(key);
        if (!opt) return nullptr;
        auto* chunk = reinterpret_cast<Chunk*>(*opt);
        ++pinned_[chunk];
        return chunk;
    }

    void releasePinned(const Chunk* chunk) noexcept {
        if (!chunk) return;
        auto* c = const_cast<Chunk*>(chunk);
        std::lock_guard lock(pin_mutex_);
        auto it = pinned_.find(c);
        if (it == pinned_.end()) return;
        if (--it->second > 0) return;
        pinned_.erase(it);
        bool deferred = pending_evict_.contains(c);
        pending_evict_.erase(c);
        if (deferred) {
            std::lock_guard plk(pool_mutex_);
            if (pool_.size() < POOL_CAP) {
                pool_.push_back(c);
            } else {
                delete c;
            }
        }
    }

    void put(int64_t key, Chunk* chunk) noexcept {
        cache_.put(key, reinterpret_cast<uintptr_t>(chunk));
    }

    void erase(int64_t key) noexcept {
        cache_.erase(key);
    }

    Chunk* takeFromPool() noexcept {
        std::lock_guard lock(pool_mutex_);
        if (pool_.empty()) return nullptr;
        Chunk* chunk = pool_.back();
        pool_.pop_back();
        return chunk;
    }

    void clear() noexcept {
        cache_.clear();
        std::lock_guard lock(pool_mutex_);
        for (auto* chunk : pool_) delete chunk;
        pool_.clear();
    }

private:
    mutable ClockCache<uintptr_t, 1024> cache_;
    std::vector<Chunk*> pool_;
    mutable std::mutex pool_mutex_;
    std::unordered_map<Chunk*, int> pinned_;
    std::unordered_set<Chunk*> pending_evict_;
    mutable std::mutex pin_mutex_;
};
