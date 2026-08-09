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

uint64_t WorkbenchStateManager::posKey(int32_t x, int32_t y, int32_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 0) |
           (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) |
           (static_cast<uint64_t>(static_cast<uint16_t>(z)) << 48);
}

std::vector<uint8_t> WorkbenchStateManager::serializeGrid(
    const std::vector<RecipeManager::ItemStack>& grid) const
{
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
    size_t n = (std::min)(data.size() / 5, size_t{9});
    for (size_t i = 0; i < n; ++i) {
        size_t off = i * 5;
        grid.push_back({
            static_cast<uint16_t>(data[off + 0] | (static_cast<uint16_t>(data[off + 1]) << 8)),
            data[off + 2],
            static_cast<uint16_t>(data[off + 3] | (static_cast<uint16_t>(data[off + 4]) << 8))
        });
    }
    return grid;
}

void WorkbenchStateManager::setGridState(
    int32_t x, int32_t y, int32_t z,
    const std::vector<RecipeManager::ItemStack>& grid)
{
    uint64_t key = posKey(x, y, z);
    grids_[key] = grid;

    if (essClient_ && essClient_->IsConnected()) {
        auto serialized = serializeGrid(grid);
        essClient_->SaveEntityState(
            dimension_, x, y, z,
            2,  // entity_type = Workbench (core.fbs: 2=Workbench)
            serialized,
            [](bool success) {
                if (!success) {
                    spdlog::warn("WorkbenchStateManager: SaveEntityState failed");
                }
            });
    }
}

void WorkbenchStateManager::getGridState(
    int32_t x, int32_t y, int32_t z, LoadCallback callback)
{
    uint64_t key = posKey(x, y, z);
    auto it = grids_.find(key);
    if (it != grids_.end()) {
        callback(it->second);
        return;
    }
    // Cache miss: load from EntityStateStore.
    if (essClient_ && essClient_->IsConnected()) {
        essClient_->LoadEntityState(
            dimension_, x, y, z, 2,
            [this, key, callback](const simcore::EntityStateStoreClient::EntityStateData& state) {
                if (state.state.empty()) {
                    callback({});
                    return;
                }
                auto grid = deserializeGrid(state.state);
                grids_[key] = grid;
                callback(grid);
            });
    } else {
        callback({});
    }
}

void WorkbenchStateManager::removeGridState(int32_t x, int32_t y, int32_t z) {
    uint64_t key = posKey(x, y, z);
    grids_.erase(key);

    if (essClient_ && essClient_->IsConnected()) {
        essClient_->SaveEntityState(
            dimension_, x, y, z, 2, {},
            [](bool success) {
                if (!success) {
                    spdlog::warn("WorkbenchStateManager: failed to clear state");
                }
            });
    }
}

}  // namespace simulation_core
