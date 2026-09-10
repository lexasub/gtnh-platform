#include "ResourceBufferStateStore.h"

#include <common/ResourceBufferStateCodec.h>
#include <common/coords/Coords.h>

#include <spdlog/spdlog.h>

ResourceBufferStateStore::ResourceBufferStateStore() = default;

bool ResourceBufferStateStore::Enqueue(
    const std::shared_ptr<std::vector<uint8_t>>& payload) {
  if (!payload || payload->empty()) return false;

  gtnh::common::ResourceBufferStateMsg msg;
  if (!gtnh::common::ParseResourceBufferState(payload->data(),
                                              payload->size(), &msg)) {
    spdlog::warn("ResourceBufferStateStore: dropped malformed update ({} bytes)",
                 payload->size());
    return false;
  }

  QueuedUpdate queued;
  queued.entry.owner_id = msg.owner_id;
  queued.entry.port_id = msg.port_id;
  queued.entry.resource_kind = static_cast<uint8_t>(msg.resource_kind);
  queued.entry.resource_id = msg.resource_id;
  queued.entry.amount = msg.amount;
  queued.entry.capacity = msg.capacity;
  queued.entry.rate = msg.rate;
  queued.entry.epoch = msg.epoch;
  queued.entry.sequence = msg.sequence;
  queued.entry.pos = BlockPos{msg.x, msg.y, msg.z};
  queued.removed = msg.removed;

  std::lock_guard<std::mutex> lock(mutex_);
  pending_.push_back(std::move(queued));
  return true;
}

void ResourceBufferStateStore::ApplyPending() {
  std::deque<QueuedUpdate> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(pending_);
  }

  for (auto& queued : batch) {
    const Entry& update = queued.entry;
    const PortKey key{update.owner_id, update.port_id};

    auto removed_it = removed_.find(key);
    if (removed_it != removed_.end()) {
      if (update.epoch <= removed_it->second) {
        spdlog::debug("ResourceBufferStateStore: dropped update for removed port owner={} port={} epoch={}",
                      update.owner_id, update.port_id, update.epoch);
        continue;
      }
      // Port re-registered with a newer generation.
      removed_.erase(removed_it);
    }

    auto it = entries_.find(key);
    if (queued.removed) {
      if (it != entries_.end() && update.epoch < it->second.epoch) {
        continue;  // stale removal
      }
      if (it != entries_.end()) {
        pos_index_.erase(MakeBlockPosKey(it->second.pos.x, it->second.pos.y,
                                         it->second.pos.z));
        entries_.erase(it);
      }
      removed_[key] = update.epoch;
      continue;
    }

    if (it != entries_.end()) {
      const Entry& current = it->second;
      if (update.epoch < current.epoch) {
        spdlog::debug("ResourceBufferStateStore: dropped stale epoch owner={} port={} ({} < {})",
                      update.owner_id, update.port_id, update.epoch,
                      current.epoch);
        continue;
      }
      if (update.epoch == current.epoch &&
          update.sequence <= current.sequence) {
        spdlog::debug("ResourceBufferStateStore: dropped out-of-order update owner={} port={} seq={} (current {})",
                      update.owner_id, update.port_id, update.sequence,
                      current.sequence);
        continue;
      }
      if (update.epoch > current.epoch) {
        pos_index_.erase(MakeBlockPosKey(current.pos.x, current.pos.y,
                                         current.pos.z));
      }
    }

    Apply(update);
  }
}

void ResourceBufferStateStore::Apply(const Entry& update) {
  PortKey key{update.owner_id, update.port_id};
  pos_index_[MakeBlockPosKey(update.pos.x, update.pos.y, update.pos.z)] = key;
  entries_[key] = update;
}

const ResourceBufferStateStore::Entry* ResourceBufferStateStore::FindAt(
    const BlockPos& pos) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = pos_index_.find(MakeBlockPosKey(pos.x, pos.y, pos.z));
  if (it == pos_index_.end()) return nullptr;
  auto entry = entries_.find(it->second);
  return entry == entries_.end() ? nullptr : &entry->second;
}

std::vector<ResourceBufferStateStore::Entry>
ResourceBufferStateStore::FindAllAt(const BlockPos& pos) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t posKey = MakeBlockPosKey(pos.x, pos.y, pos.z);
  std::vector<Entry> result;
  for (const auto& [key, entry] : entries_) {
    if (MakeBlockPosKey(entry.pos.x, entry.pos.y, entry.pos.z) == posKey)
      result.push_back(entry);
  }
  return result;
}

void ResourceBufferStateStore::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.clear();
  entries_.clear();
  pos_index_.clear();
  removed_.clear();
}

void ResourceBufferStateStore::ClearChunk(const ChunkCoord& coord) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    const BlockPos& pos = it->second.pos;
    if ((pos.x >> 5) == coord.x && (pos.y >> 5) == coord.y &&
        (pos.z >> 5) == coord.z) {
      pos_index_.erase(MakeBlockPosKey(pos.x, pos.y, pos.z));
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

size_t ResourceBufferStateStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}
