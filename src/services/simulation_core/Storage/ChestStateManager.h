#pragma once
// ChestStateManager — load/save chest slots as a Protocol::MachineState blob
// (entity_type 3) via EntityStateStoreClient. Cache-first (posKey), async ESS
// on miss. Shared by the chest-open handler (load) and the close handler
// (persist). The container session (ContainerSessionRegistry) is the live
// per-player copy; this manager is the persistence layer behind it.

#include "Storage/ContainerSession.h"
#include "Storage/PlayerInventoryStore.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace simcore {

class EntityStateStoreClient;

class ChestStateManager {
public:
  using LoadCallback = std::function<void(const std::vector<PersistSlot>&)>;

  ChestStateManager(std::shared_ptr<EntityStateStoreClient> essClient,
                    int32_t dimension);
  ~ChestStateManager() = default;

  // Load container slots: cache-first, async from EntityStateStore on miss.
  // entity_type defaults to kChestEntityType; machines pass their machine_id
  // so the ESS key (dim,x,y,z,entity_type) is correct.
  void loadSlots(int32_t x, int32_t y, int32_t z, LoadCallback cb,
                 uint16_t entity_type = kChestEntityType);
  // Persist container slots to EntityStateStore immediately.
  void saveSlots(int32_t x, int32_t y, int32_t z,
                 const std::vector<PersistSlot>& slots,
                 uint16_t entity_type = kChestEntityType);
  // Clear cache + persist empty state (block destroyed).
  void clearSlots(int32_t x, int32_t y, int32_t z,
                  uint16_t entity_type = kChestEntityType);

private:
  static uint64_t posKey(int32_t x, int32_t y, int32_t z);

  std::unordered_map<uint64_t, std::vector<PersistSlot>> cache_;
  std::shared_ptr<EntityStateStoreClient> essClient_;
  int32_t dimension_;
};

// ── MachineState blob helpers (shared by session open/close) ────────────────

// Encode chest slots into a Protocol::MachineState blob (MachineInventory).
std::vector<uint8_t> EncodeChestBlob(const std::vector<PersistSlot>& slots);

// Decode a Protocol::MachineState blob into chest slots (empty on parse fail).
std::vector<PersistSlot> DecodeChestBlob(const std::vector<uint8_t>& blob);

} // namespace simcore

// ── Shared snapshot publisher (open / click / close) ───────────────────────
namespace simcore {
class IoUringRouterClient;
class PlayerInventoryStore;
class ContainerSessionRegistry;

// Build and publish the full player.inventory.update snapshot with container_id=1
// (cursor + 40 player slots + the player's open container slots). Thread-safe
// (reads store/registry under their mutexes, publishes via router).
// Callers holding the sessions lock (inside forEachOpenAt lambdas) MUST use the
// overload that takes ContainerSession& to avoid re-locking the same std::mutex.
void PublishFullInventory(std::shared_ptr<IoUringRouterClient> router,
                          PlayerInventoryStore& store,
                          ContainerSessionRegistry& sessions,
                          uint64_t pid, int32_t x, int32_t y, int32_t z);

// Variant for callers that already hold a ContainerSession reference and do NOT
// need the registry lookup (forEachOpenAt lambdas, InventoryActionHandler).
void PublishFullInventory(std::shared_ptr<IoUringRouterClient> router,
                          PlayerInventoryStore& store,
                          ContainerSession& sess,
                          uint64_t pid, int32_t x, int32_t y, int32_t z);
} // namespace simcore
