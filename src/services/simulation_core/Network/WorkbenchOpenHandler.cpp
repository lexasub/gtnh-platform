#include "WorkbenchOpenHandler.h"
#include "../Storage/ContainerSession.h"
#include "../Storage/ChestStateManager.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

void WorkbenchOpenHandler::handle(const std::vector<uint8_t>& data) {
    if (!wbStateManager_ || !eventPublisher_) return;

    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) {
        spdlog::warn("[WorkbenchOpen] invalid ContainerOpenReq payload");
        return;
    }
    auto* req = flatbuffers::GetRoot<Protocol::ContainerOpenReq>(data.data());
    if (!req || !req->pos()) return;
    uint64_t pid = req->player_id();
    int32_t x = req->pos()->x(), y = req->pos()->y(), z = req->pos()->z();

    spdlog::info("[WorkbenchOpen] player={} open workbench at ({},{},{})", pid, x, y, z);

    // Register session IMMEDIATELY on the main thread (empty 3×3 grid).
    // Same invariant as MachineOpenHandler: session must exist before any
    // click arrives. getGridState is async on cold cache (ESS RPC).
    if (chestSessions_ && inventoryStore_ && router_) {
        ContainerSession sess;
        sess.kind = ContainerSession::Kind::Workbench;
        sess.x = x; sess.y = y; sess.z = z;
        sess.slots.assign(9, PersistSlot{});
        chestSessions_->open(pid, std::move(sess));
        PublishFullInventory(router_, *inventoryStore_, *chestSessions_,
                             pid, x, y, z);
    }

    wbStateManager_->getGridState(x, y, z, [this, pid, x, y, z]
        (const std::vector<RecipeManager::ItemStack>& grid) {
            spdlog::debug("[WorkbenchOpen] loaded grid at ({},{},{}) ({} slots)", x, y, z, grid.size());
            if (chestSessions_ && inventoryStore_ && router_) {
                ContainerSession* s = chestSessions_->find(pid);
                if (s && !grid.empty()) {
                    // Overwrite the pre-sized 9 slots (preserve size —
                    // cold cache returns empty grid, which would shrink
                    // slots to 0 and make every click a no-op).
                    size_t n = std::min(grid.size(), s->slots.size());
                    for (size_t i = 0; i < n; ++i) {
                        s->slots[i] = PersistSlot{grid[i].item_id, grid[i].count,
                                                  grid[i].metadata};
                    }
                }
                PublishFullInventory(router_, *inventoryStore_, *chestSessions_,
                                     pid, x, y, z);
            }
            eventPublisher_->publishGridUpdate(x, y, z, grid);
        });
}

} // namespace simcore
