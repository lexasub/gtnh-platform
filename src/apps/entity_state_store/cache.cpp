#include "cache.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace gtnh {
namespace entity_state_store {

Cache::Cache(size_t max_size)
    : max_size_(max_size) {}

Cache::~Cache() {
    shutdown();
}

void Cache::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

std::string Cache::makeKey(uint32_t dimension, int32_t x, int32_t y, int32_t z) {
    return std::to_string(dimension) + "|" + 
           std::to_string(x) + "|" + 
           std::to_string(y) + "|" + 
           std::to_string(z);
}

bool Cache::get(const std::string& key, std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        data = it->second.data;
        return true;
    }
    return false;
}

void Cache::set(const std::string& key, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[key] = {data, std::chrono::steady_clock::now()};
    evictIfNeeded();
}

void Cache::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(key);
}

void Cache::evictIfNeeded() {
    if (cache_.size() > max_size_) {
        size_t toRemove = cache_.size() - max_size_ + 10;
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.begin();
        for (size_t i = 0; i < toRemove && it != cache_.end(); ++i, ++it) {
            cache_.erase(it++);
        }
        
        spdlog::debug("Cache evicted {} entries", toRemove);
    }
}

} // namespace entity_state_store
} // namespace gtnh
