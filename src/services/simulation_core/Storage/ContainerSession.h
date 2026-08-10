#pragma once
// ContainerSession.h — per-player open-container session for the
// server-authoritative click model (Phase B chest, Phase C machine).
//
// One active container per player (container_id=1): chest in Phase B, machine
// in Phase C, workbench later. State is loaded from EntityStateStore on open
// (async, callback fires on the ESS io thread) and written back on every
// mutation and on close. Chest slots persist as a Protocol::MachineState blob
// (entity_type 3); machine slots persist under the same blob format keyed by
// entity_type = machine_id — the format the legacy machine path used.
//
// Chest sessions own a COPY of the slots. Machine sessions reference the LIVE
// ECS InventoryContainer component on the machine entity (Kind::Machine): the
// stored handle is {registry*, entity} and every access re-resolves
// reg.try_get<InventoryContainer>() — a cached pointer would dangle because
// EnTT packed storage can relocate the component across ticks and block-clear
// removes it (SimulationEngine onBlockChanged). All machine-session access
// must happen on the main-queue thread where the ECS registry is mutated.
//
// Thread contract: open() (via the io-thread load callback) and close() write
// under the mutex; the click handler reads via find()/get() on the main-queue
// thread. The returned pointer from find() stays valid while the session is
// open (the click handler must not outlive it across a close()). Machine
// slot-rule mutation never resizes the container vector (rules bounds-check),
// so element pointers stay valid within one main-thread callback.

#include "ECS/components/InventoryContainer.h"
#include "Storage/PlayerInventoryStore.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace simcore {

// Chest EntityStateStore entity_type (matches kChestEntityType, shared here so
// both the interact handler and the container session agree).
inline constexpr uint16_t kChestEntityType = 3;

// InventorySlot and PersistSlot must stay byte-identical: machine sessions
// reinterpret the live ECS vector as std::vector<PersistSlot>* so the pure
// click rule table (InventoryClick.h) can mutate it with zero changes.
static_assert(sizeof(InventorySlot) == sizeof(PersistSlot) &&
                  alignof(InventorySlot) == alignof(PersistSlot) &&
                  offsetof(InventorySlot, item_id) ==
                      offsetof(PersistSlot, item_id) &&
                  offsetof(InventorySlot, count) ==
                      offsetof(PersistSlot, count) &&
                  offsetof(InventorySlot, meta) ==
                      offsetof(PersistSlot, meta),
              "InventorySlot and PersistSlot layouts must stay identical");

struct ContainerSession {
  enum class Kind : uint8_t { Chest, Machine, Workbench };

  Kind kind = Kind::Chest;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  uint16_t entity_type = 0; // 3=chest; machine_id for machine (ESS key)
  std::vector<PersistSlot> slots; // chest: owned copy (unchanged)
  entt::registry* reg = nullptr; // machine only
  entt::entity machine_entity = entt::null; // machine: handle, NEVER a cached InventoryContainer*

  bool isMachine() const { return kind == Kind::Machine; }

  // Live slot view for the click rules. Machine: re-resolves the ECS
  // InventoryContainer and reinterprets its slots as PersistSlot. Chest: the
  // owned copy. Falls back to the owned copy when the ECS link is not yet
  // established (session registered on main thread before async ESS load
  // completes) or when the entity is gone (block cleared while window open).
  std::vector<PersistSlot>* slotsRef() {
    if (!isMachine()) return &slots;
    if (!reg) return &slots; // ECS link pending (async load in flight)
    auto* c = reg->try_get<InventoryContainer>(machine_entity);
    if (!c) return &slots; // entity destroyed while window open
    return reinterpret_cast<std::vector<PersistSlot>*>(&c->slots);
  }

  const std::vector<PersistSlot>* slotsRef() const {
    if (!isMachine()) return &slots;
    if (!reg) return &slots;
    auto* c = reg->try_get<InventoryContainer>(machine_entity);
    if (!c) return &slots;
    return reinterpret_cast<const std::vector<PersistSlot>*>(&c->slots);
  }
};

class ContainerSessionRegistry {
public:
  // Register (or replace) the player's open container.
  void open(uint64_t pid, ContainerSession s) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[pid] = std::move(s);
  }

  // Copy of the player's session for publish; false if none open.
  bool get(uint64_t pid, ContainerSession &out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(pid);
    if (it == sessions_.end()) return false;
    out = it->second;
    return true;
  }

  // Mutable view for the click handler (caller holds no lock; the session is
  // stable while open because close() is main-thread-only).
  ContainerSession *find(uint64_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(pid);
    return it == sessions_.end() ? nullptr : &it->second;
  }

  // Deregister after persist-on-close.
  void close(uint64_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(pid);
  }

  // Invoke fn(pid, session) for every player with an open session at the given
  // block position. Used by flows that have no player id (ItemFlowHandler,
  // MachineSystem ticks) to notify open windows. fn runs with the registry
  // lock held — do not call back into open()/close() from fn.
  template <typename Fn>
  void forEachOpenAt(int32_t x, int32_t y, int32_t z, Fn&& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [pid, s] : sessions_) {
      if (s.x == x && s.y == y && s.z == z) fn(pid, s);
    }
  }

private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, ContainerSession> sessions_;
};

} // namespace simcore
