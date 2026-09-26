#pragma once
#include <game/recipes/RecipeManager.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace simcore {
class EntityStateStoreClient;
}

namespace simulation_core {
class WorkbenchStateManager {
public:
  using LoadCallback =
      std::function<void(const std::vector<RecipeManager::ItemStack> &)>;

  WorkbenchStateManager(
      std::shared_ptr<simcore::EntityStateStoreClient> essClient,
      int32_t dimension);
  ~WorkbenchStateManager() = default;

  // Store grid state for a workbench at block position (x,y,z).
  // Also persists to EntityStateStore immediately.
  void setGridState(int32_t x, int32_t y, int32_t z,
                    const std::vector<RecipeManager::ItemStack> &grid);
  // Get grid state: cache-first, loads from EntityStateStore on miss.
  // Callback is invoked synchronously if cached, or asynchronously after ESS
  // load.
  void getGridState(int32_t x, int32_t y, int32_t z, LoadCallback callback);
  // Remove state when workbench is destroyed (clears cache + ESS).
  void removeGridState(int32_t x, int32_t y, int32_t z);

private:
  // Serialize 9-item grid to 45-byte buffer: [item_id:uint16, count:uint8,
  // metadata:uint16] x 9
  std::vector<uint8_t>
  serializeGrid(const std::vector<RecipeManager::ItemStack> &grid) const;
  // Deserialize 45-byte buffer back to 9 ItemStacks
  std::vector<RecipeManager::ItemStack>
  deserializeGrid(const std::vector<uint8_t> &data) const;

  // Cache key for the whole block position, held as a struct.
  //
  // It used to be a single uint64_t packed as
  // (x << 0) | (y << 32) | ((uint16_t)z << 48) (gp-mhiv), which was wrong in
  // two ways at once: z was truncated to 16 bits, AND that 16-bit field sat at
  // bits 48-63 where y's 32-bit field also lives (bits 32-63), so z
  // overlapped y. (0,0,0) and (0,0,65536) therefore shared one entry, as did
  // (5,64,1) and (5,65600,1) — a player opening one workbench saw, and
  // overwrote, the other's crafting grid.
  //
  // Widening the fields cannot fix this in general. A position is three int32
  // axes = 96 bits and the key is 64, so SOME pair of coordinates must
  // collide; only a real 3-tuple key is exact. The hash below only picks a
  // bucket — operator== separates any true hash collision, so the map itself
  // stays exact for every representable coordinate.
  struct PosKey {
    int32_t x, y, z;
    bool operator==(const PosKey& other) const {
      return x == other.x && y == other.y && z == other.z;
    }
  };

  struct PosKeyHash {
    size_t operator()(const PosKey& k) const noexcept {
      // FNV-1a over the three axes' 32-bit patterns, seeded so that (0,0,0)
      // does not hash to 0. Cheap, allocation-free, and mixes every axis bit.
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
      return static_cast<size_t>(h);
    }
  };

  static PosKey posKey(int32_t x, int32_t y, int32_t z);

  std::unordered_map<PosKey, std::vector<RecipeManager::ItemStack>, PosKeyHash>
      grids_;
  std::shared_ptr<simcore::EntityStateStoreClient> essClient_;
  int32_t dimension_;
};
} // namespace simulation_core