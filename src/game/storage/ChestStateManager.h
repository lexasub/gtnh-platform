#pragma once
// ChestStateManager — load/save chest slots as a Protocol::MachineState blob
// (entity_type 3) via EntityStateStoreClient. Cache-first (posKey), async ESS
// on miss. Shared by the chest-open handler (load) and the close handler
// (persist). The container session (ContainerSessionRegistry) is the live
// per-player copy; this manager is the persistence layer behind it.

#include "ContainerSession.h"
#include "PlayerInventoryStore.h"
#include <cstddef>
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
  // The cache key is the whole block position PLUS the entity type.
  //
  // It used to be a single uint64_t packed as (x << 32) ^ (y << 16) ^ z
  // (gp-5t4d), which OVERLAPPED the y and z fields — y was shifted only 16
  // bits and z was XORed in unshifted, so bit 16 of y landed on bit 0 of z.
  // posKey(0,0,0) and posKey(0,1,65536) both evaluated to 0, so two real
  // chests shared one entry: opening the second showed the first one's items,
  // and breaking either emptied the other.
  //
  // Widening the fields cannot fix this in general. A position is three int32
  // axes = 96 bits, and the key is 64, so SOME pair of coordinates must
  // collide; only a real 3-tuple key is exact. The hash below only picks a
  // bucket — operator== separates any true hash collision, so the map itself
  // stays exact for every representable coordinate.
  //
  // entity_type joined the key for the same reason (gp-py5s): the cache has to
  // agree with EntityStateStore, whose key is (dim, x, y, z, entity_type). A
  // chest (3) and a machine (its own machine_id) can occupy the same block
  // position — a chest inside a machine's footprint, or a machine placed in a
  // chest position — and they are separate entities with separate persisted
  // state. Keyed on position alone they shared one entry, so opening the chest
  // returned the machine's slots and the machine's save overwrote the chest's.
  struct PosKey {
    int32_t x, y, z;
    uint16_t entity_type;
    bool operator==(const PosKey& other) const {
      return x == other.x && y == other.y && z == other.z &&
             entity_type == other.entity_type;
    }
  };

  struct PosKeyHash {
    size_t operator()(const PosKey& k) const noexcept {
      // FNV-1a over the three axes' 32-bit patterns plus the 16-bit entity
      // type, seeded so that (0,0,0) does not hash to 0. Cheap, allocation-free,
      // and mixes every field's bits.
      uint64_t h = 1469598103934665603ull;
      const uint32_t axes[3] = {static_cast<uint32_t>(k.x),
                                static_cast<uint32_t>(k.y),
                                static_cast<uint32_t>(k.z)};
      for (const uint32_t a : axes) {
        for (int byte = 0; byte < 4; ++byte) {
          h ^= static_cast<uint64_t>((a >> (byte * 8)) & 0xFFu);
          h *= 1099511628211ull;
        }
      }
      for (int byte = 0; byte < 2; ++byte) {
        h ^= static_cast<uint64_t>((k.entity_type >> (byte * 8)) & 0xFFu);
        h *= 1099511628211ull;
      }
      return static_cast<size_t>(h);
    }
  };

  static PosKey posKey(int32_t x, int32_t y, int32_t z, uint16_t entity_type);

  std::unordered_map<PosKey, std::vector<PersistSlot>, PosKeyHash> cache_;
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
