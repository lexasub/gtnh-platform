#pragma once

#include "Common/Types.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Client-side store for PipeNetwork debug contents queries (pipe.contents.*).
//
// Last-write-wins by BlockPos: every verified PipeContentsResp replaces the
// previous snapshot for that position. There is no epoch/sequence gating here
// — the query is a debug snapshot whose only ordering requirement is "newest
// reply wins". A `found=false` reply (no pipe node at pos) is stored, so the
// overlay can distinguish "queried, no pipe" from "never queried" (nullptr).
// ---------------------------------------------------------------------------
class PipeContentsStateStore {
 public:
  struct Entry {
    BlockPos pos{};
    uint64_t node_id = 0;
    uint32_t fluid_id = 0;  // 0 = empty / no fluid
    int32_t amount = 0;
    int32_t capacity = 0;
    bool found = false;
  };

  PipeContentsStateStore();

  // Network side: verify + parse the wire payload and enqueue the result.
  // Malformed buffers are dropped here (returns false).
  bool Enqueue(const std::shared_ptr<std::vector<uint8_t>>& payload);

  // Render thread: drain the queue. Call once per frame before UI rendering.
  void ApplyPending();

  // Snapshot for the block at `pos`, or nullptr when no reply has arrived.
  // The returned pointer is valid until the next ApplyPending/Clear.
  [[nodiscard]] const Entry* FindAt(const BlockPos& pos) const;

  // Drop all state: reconnect or shutdown.
  void Clear();

  [[nodiscard]] size_t size() const;

 private:
  void Apply(const Entry& update);

  mutable std::mutex mutex_;
  std::deque<Entry> pending_;
  std::unordered_map<uint64_t, Entry> entries_;
};