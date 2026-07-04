#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gtnh {
namespace entity_state_store {

struct CachedEntry {
  std::vector<uint8_t> data;
  std::chrono::steady_clock::time_point timestamp;
};

class Cache {
public:
  Cache(size_t max_size = 1000);
  ~Cache();

  void shutdown();

  std::string makeKey(uint32_t dimension, int32_t x, int32_t y, int32_t z);
  bool get(const std::string &key, std::vector<uint8_t> &data);
  void set(const std::string &key, const std::vector<uint8_t> &data);
  void remove(const std::string &key);

private:
  void evictIfNeeded();

  std::unordered_map<std::string, CachedEntry> cache_;
  std::mutex mutex_;
  size_t max_size_;
};

} // namespace entity_state_store
} // namespace gtnh
