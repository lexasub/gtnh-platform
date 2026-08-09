#include "WorkbenchOpenHandler.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

void WorkbenchOpenHandler::handle(const std::vector<uint8_t>& data) {
    if (!wbStateManager_ || !eventPublisher_) return;

    auto pos = flatbuffers::GetRoot<Protocol::Vec3i>(data.data());
    if (!pos) {
        spdlog::warn("[WorkbenchOpen] invalid Vec3i payload");
        return;
    }
    int32_t x = pos->x(), y = pos->y(), z = pos->z();

    wbStateManager_->getGridState(x, y, z, [this, x, y, z]
        (const std::vector<RecipeManager::ItemStack>& grid) {
            spdlog::debug("[WorkbenchOpen] loaded grid at ({},{},{}) ({} slots)", x, y, z, grid.size());
            eventPublisher_->publishGridUpdate(x, y, z, grid);
        });
}

} // namespace simcore
