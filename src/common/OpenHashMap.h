#pragma once
// OpenHashMap: open-addressed hash map with linear probing.
// Zero heap allocation, O(1) average lookup.
// Key must be equality-comparable. SentinelKey marks empty slots.
#include <cstdint>
template <typename Key, typename Value, int Capacity, Key SentinelKey>
class OpenHashMap {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
  static_assert(Capacity >= 2, "Capacity must be >= 2");

public:
  struct Entry {
    Key key = SentinelKey;
    Value value{};
  };

  constexpr OpenHashMap() = default;

  Value* insert(Key key, const Value& value) {
    if (key == SentinelKey)
      return nullptr;
    uint32_t idx = hash(key);
    for (int i = 0; i < Capacity; ++i) {
      if (entries_[idx].key == SentinelKey || entries_[idx].key == key) {
        bool isNew = (entries_[idx].key == SentinelKey);
        entries_[idx].key = key;
        entries_[idx].value = value;
        if (isNew)
          ++size_;
        return &entries_[idx].value;
      }
      idx = (idx + 1) & mask_;
    }
    return nullptr;
  }

  const Value* find(Key key) const {
    if (key == SentinelKey)
      return nullptr;
    uint32_t idx = hash(key);
    for (int i = 0; i < Capacity; ++i) {
      if (entries_[idx].key == key)
        return &entries_[idx].value;
      if (entries_[idx].key == SentinelKey)
        return nullptr;
      idx = (idx + 1) & mask_;
    }
    return nullptr;
  }

  Value* find(Key key) {
    return const_cast<Value*>(static_cast<const OpenHashMap*>(this)->find(key));
  }

  bool erase(Key key) {
    if (key == SentinelKey)
      return false;
    uint32_t idx = hash(key);
    for (int i = 0; i < Capacity; ++i) {
      if (entries_[idx].key == key) {
        entries_[idx].key = SentinelKey;
        entries_[idx].value = Value{};
        --size_;
        // Backward-shift deletion: close the probe-chain hole left by the erased
        // slot by sliding each following entry back into the gap, as long as the
        // gap lies on that entry's probe path [ideal, next] (circularly). Each
        // step is O(1); entries whose home is outside that path keep the map's
        // probe clusters contiguous, so linear-probing find() stays correct.
        uint32_t hole = idx;
        uint32_t next = (hole + 1) & mask_;
        while (entries_[next].key != SentinelKey) {
          uint32_t ideal = hash(entries_[next].key);
          // Is `hole` on the probe path from `ideal` to `next` (inclusive)?
          bool on_path = (ideal <= next) ? (ideal <= hole && hole <= next)
                                         : (hole >= ideal || hole <= next);
          // Shift `next` back into the gap only if doing so keeps it on its own
          // probe path. Entries whose home is outside [ideal, next] must NOT move
          // (they'd become unfindable), but we keep scanning: a later entry whose
          // probe path does include `hole` can still close the gap. Never break —
          // breaking leaves an internal hole that breaks find() for displaced keys.
          if (on_path) {
            entries_[hole] = entries_[next];
            entries_[next].key = SentinelKey;
            entries_[next].value = Value{};
            hole = next;
          }
          next = (next + 1) & mask_;
        }
        return true;
      }
      idx = (idx + 1) & mask_;
    }
    return false;
  }

  constexpr int size() const { return size_; }
  constexpr int capacity() const { return Capacity; }
  constexpr bool empty() const { return size_ == 0; }

  template <typename Fn> void for_each(Fn&& fn) const {
    for (int i = 0; i < Capacity; ++i) {
      if (entries_[i].key != SentinelKey)
        fn(entries_[i].key, entries_[i].value);
    }
  }

  void clear() {
    for (int i = 0; i < Capacity; ++i) {
      entries_[i].key = SentinelKey;
      entries_[i].value = Value{};
    }
    size_ = 0;
  }

private:
  static constexpr uint32_t mask_ = Capacity - 1;
  int size_ = 0;
  Entry entries_[Capacity]{};

  // Knuth multiplicative hash
  static constexpr uint32_t hash(Key key) {
    uint32_t k = static_cast<uint32_t>(key);
    k *= 2654435761u;
    return (k >> (32 - __builtin_ctz(Capacity))) & mask_;
  }
};
