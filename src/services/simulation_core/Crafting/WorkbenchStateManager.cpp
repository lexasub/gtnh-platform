#include "WorkbenchStateManager.h"
#include "Network/clients/EntityStateStoreClient.h"
#include <spdlog/spdlog.h>

namespace simulation_core {

WorkbenchStateManager::WorkbenchStateManager(
    std::shared_ptr<simcore::EntityStateStoreClient> essClient,
    int32_t dimension)
    : essClient_(std::move(essClient))
    , dimension_(dimension)
{
}

std::vector<uint8_t> WorkbenchStateManager::serializeGrid(
    const std::vector<RecipeManager::ItemStack>& grid) const
{
    // 45-byte format: 9 items x 5 bytes = [item_id:uint16, count:uint8, meta:uint16]
    std::vector<uint8_t> data(45, 0);
    size_t n = (std::min)(grid.size(), size_t{9});
    for (size_t i = 0; i < n; ++i) {
        auto& item = grid[i];
        size_t off = i * 5;
        data[off + 0] = static_cast<uint8_t>(item.item_id & 0xFF);
        data[off + 1] = static_cast<uint8_t>((item.item_id >> 8) & 0xFF);
        data[off + 2] = item.count;
        data[off + 3] = static_cast<uint8_t>(item.metadata & 0xFF);
        data[off + 4] = static_cast<uint8_t>((item.metadata >> 8) & 0xFF);
    }
    return data;
}

std::vector<RecipeManager::ItemStack> WorkbenchStateManager::deserializeGrid(
    const std::vector<uint8_t>& data) const
{
    std::vector<RecipeManager::ItemStack> grid;
    grid.reserve(9);
    size_t available = data.size() / 5;
    size_t n = (std::min)(available, size_t{9});
    for (size_t i = 0; i < n; ++i) {
        size_t off = i * 5;
        RecipeManager::ItemStack item;
        item.item_id  = static_cast<uint16_t>(data[off + 0] | (static_cast<uint16_t>(data[off + 1]) << 8));
        item.count    = data[off + 2];
        item.metadata = static_cast<uint16_t>(data[off + 3] | (static_cast<uint16_t>(data[off + 4]) << 8));
        grid.push_back(item);
    }
    return grid;
}

std::vector<RecipeManager::ItemStack> WorkbenchStateManager::getGridState(
    uint64_t entityId, int32_t x, int32_t y, int32_t z)
{
    (void)x; (void)y; (void)z;
    auto it = grids_.find(entityId);
    if (it != grids_.end()) {
        return it->second;
    }
    return std::vector<RecipeManager::ItemStack>();
}

void WorkbenchStateManager::setGridState(
    uint64_t entityId, int32_t x, int32_t y, int32_t z,
    const std::vector<RecipeManager::ItemStack>& grid)
{
    (void)x; (void)y; (void)z;
    grids_[entityId] = grid;

    if (essClient_ && essClient_->IsConnected()) {
        auto serialized = serializeGrid(grid);
        essClient_->SaveEntityState(
            dimension_, static_cast<int32_t>(entityId),
            static_cast<int32_t>(entityId >> 16),
            static_cast<int32_t>(entityId >> 32),
            static_cast<uint16_t>(entityId & 0xFFFF),
            serialized,
            [](bool success) {
                if (!success) {
                    spdlog::warn("WorkbenchStateManager: SaveEntityState failed");
                }
            });
    }
}

void WorkbenchStateManager::removeGridState(
    uint64_t entityId, int32_t x, int32_t y, int32_t z)
{
    (void)x; (void)y; (void)z;
    grids_.erase(entityId);

    if (essClient_ && essClient_->IsConnected()) {
        essClient_->SaveEntityState(
            dimension_, static_cast<int32_t>(entityId),
            static_cast<int32_t>(entityId >> 16),
            static_cast<int32_t>(entityId >> 32),
            static_cast<uint16_t>(entityId & 0xFFFF),
            {},
            [](bool success) {
                if (!success) {
                    spdlog::warn("WorkbenchStateManager: failed to clear state");
                }
            });
    }
}

}  // namespace simulation_core
