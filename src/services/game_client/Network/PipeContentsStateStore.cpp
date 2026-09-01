#include "Network/PipeContentsStateStore.h"

#include "core_generated.h"
#include "pipe_network_generated.h"

#include <common/coords/Coords.h>

#include <spdlog/spdlog.h>

PipeContentsStateStore::PipeContentsStateStore() = default;

bool PipeContentsStateStore::Enqueue(
    const std::shared_ptr<std::vector<uint8_t>>& payload) {
  if (!payload || payload->empty()) return false;

  flatbuffers::Verifier verifier(payload->data(), payload->size());
  if (!verifier.VerifyBuffer<Protocol::PipeContentsResp>(nullptr)) {
    spdlog::warn("PipeContentsStateStore: dropped malformed reply ({} bytes)",
                 payload->size());
    return false;
  }

  const auto* resp = flatbuffers::GetRoot<Protocol::PipeContentsResp>(payload->data());
  if (!resp || !resp->pos()) return false;

  Entry entry;
  entry.pos = BlockPos{resp->pos()->x(), resp->pos()->y(), resp->pos()->z()};
  entry.node_id = resp->node_id();
  entry.fluid_id = resp->fluid_id();
  entry.amount = resp->amount();
  entry.capacity = resp->capacity();
  entry.found = resp->found();

  std::lock_guard<std::mutex> lock(mutex_);
  pending_.push_back(std::move(entry));
  return true;
}

void PipeContentsStateStore::ApplyPending() {
  std::deque<Entry> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(pending_);
  }

  for (auto& entry : batch) Apply(entry);
}

const PipeContentsStateStore::Entry* PipeContentsStateStore::FindAt(
    const BlockPos& pos) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(MakeBlockPosKey(pos.x, pos.y, pos.z));
  return it == entries_.end() ? nullptr : &it->second;
}

void PipeContentsStateStore::Apply(const Entry& update) {
  entries_[MakeBlockPosKey(update.pos.x, update.pos.y, update.pos.z)] = update;
}

void PipeContentsStateStore::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.clear();
  entries_.clear();
}

size_t PipeContentsStateStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}