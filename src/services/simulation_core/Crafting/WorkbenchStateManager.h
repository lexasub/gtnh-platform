#pragma once
#include "RecipeManager/RecipeManager.h"
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

  // Position key for the cache: (x,y,z) packed into a single uint64_t.
  static uint64_t posKey(int32_t x, int32_t y, int32_t z);

  std::unordered_map<uint64_t, std::vector<RecipeManager::ItemStack>> grids_;
  std::shared_ptr<simcore::EntityStateStoreClient> essClient_;
  int32_t dimension_;
};
} // namespace simulation_core