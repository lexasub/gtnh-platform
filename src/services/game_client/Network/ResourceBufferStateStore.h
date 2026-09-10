#pragma once

#include "Common/Types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Client-side store for server-authoritative machine/port buffer state
// (openspec refactor-fluid-port-accounting §5, design §5 Client state).
//
// NetClient callbacks enqueue verified payloads from any thread; the render
// thread drains the queue once per frame via ApplyPending(). Updates apply
// only when their (epoch, sequence) are current for the (owner_id, port_id)
// pair — stale, out-of-order, removed-port, and malformed updates are
// discarded without touching displayed state (fail closed).
// ---------------------------------------------------------------------------
class ResourceBufferStateStore {
 public:
  struct Entry {
    uint64_t owner_id = 0;
    uint64_t port_id = 0;
    uint8_t resource_kind = 0;  // Protocol::ResourceKind raw value
    uint32_t resource_id = 0;   // canonical packed ItemId (0 = energy channel)
    int32_t amount = 0;
    int32_t capacity = 0;
    int32_t rate = 0;
    uint64_t epoch = 0;
    uint64_t sequence = 0;
    BlockPos pos{};
  };

  ResourceBufferStateStore();

  // Network side: verify + parse the wire payload and enqueue the update.
  // Malformed buffers are dropped here (returns false).
  bool Enqueue(const std::shared_ptr<std::vector<uint8_t>>& payload);

  // Render thread: drain the queue and apply every update that passes the
  // epoch/sequence gates. Call once per frame before UI rendering.
  void ApplyPending();

  // Buffer for the machine at `pos`, or nullptr. The returned pointer is
  // valid until the next ApplyPending/Clear/ClearChunk.
  [[nodiscard]] const Entry* FindAt(const BlockPos& pos) const;

  // A machine may expose multiple domains at one position (for example HU
  // input plus SU output), so callers that render the full machine UI must not
  // collapse the position to one port.
  [[nodiscard]] std::vector<Entry> FindAllAt(const BlockPos& pos) const;

  // Drop all state: reconnect (5.2.4) or shutdown.
  void Clear();

  // Drop state for machines inside an evicted/unloaded chunk (5.2.4).
  void ClearChunk(const ChunkCoord& coord);

  [[nodiscard]] size_t size() const;

 private:
  struct PortKey {
    uint64_t owner_id;
    uint64_t port_id;
    bool operator==(const PortKey& other) const {
      return owner_id == other.owner_id && port_id == other.port_id;
    }
  };
  struct PortKeyHash {
    size_t operator()(const PortKey& k) const noexcept {
      size_t h = std::hash<uint64_t>{}(k.owner_id);
      h ^= std::hash<uint64_t>{}(k.port_id) + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct QueuedUpdate {
    Entry entry;
    bool removed = false;
  };

  void Apply(const Entry& update);

  mutable std::mutex mutex_;
  std::deque<QueuedUpdate> pending_;

  // Applied state, keyed by (owner_id, port_id); indexed by position key.
  std::unordered_map<PortKey, Entry, PortKeyHash> entries_;
  std::unordered_map<uint64_t, PortKey> pos_index_;

  // Removal tombstones: (owner, port) → removal epoch. Updates with
  // epoch <= tombstone are rejected; a higher epoch re-registers the port.
  std::unordered_map<PortKey, uint64_t, PortKeyHash> removed_;
};
