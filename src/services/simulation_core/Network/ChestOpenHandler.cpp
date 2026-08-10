#include "Network/ChestOpenHandler.h"
#include "Network/clients/IoUringRouterClient.h"
#include "Storage/ChestStateManager.h"
#include "Storage/ContainerSession.h"
#include "Storage/PlayerInventoryStore.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

ChestOpenHandler::ChestOpenHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                                   std::shared_ptr<ChestStateManager> stateMgr,
                                   std::shared_ptr<PlayerInventoryStore> inv,
                                   std::shared_ptr<IoUringRouterClient> router)
    : sessions_(std::move(sessions))
    , stateMgr_(std::move(stateMgr))
    , inv_(std::move(inv))
    , router_(std::move(router)) {}

void ChestOpenHandler::handle(const std::vector<uint8_t>& data) {
  flatbuffers::Verifier v(data.data(), data.size());
  if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) return;
  auto* req = flatbuffers::GetRoot<Protocol::ContainerOpenReq>(data.data());
  if (!req || !req->pos()) return;
  uint64_t pid = req->player_id();
  auto* p = req->pos();
  if (pid == 0) return;
  int32_t x = p->x(), y = p->y(), z = p->z();

  spdlog::info("[ChestOpen] player={} open chest at ({},{},{})", pid, x, y, z);

  // Register session IMMEDIATELY on the main thread with 27 empty slots.
  // Same invariant as MachineOpenHandler: session must exist before any
  // click arrives. Chest always has 27 slots.
  ContainerSession sess;
  sess.x = x; sess.y = y; sess.z = z;
  sess.entity_type = kChestEntityType;
  sess.slots.assign(27, PersistSlot{});
  sessions_->open(pid, sess);
  PublishFullInventory(router_, *inv_, *sessions_, pid, x, y, z);

  // Async load → fill owned copy.
  stateMgr_->loadSlots(x, y, z,
      [this, pid, x, y, z](const std::vector<PersistSlot>& slots) {
        ContainerSession* s = sessions_->find(pid);
        if (s && !slots.empty()) {
          size_t n = std::min(slots.size(), s->slots.size());
          for (size_t i = 0; i < n; ++i) s->slots[i] = slots[i];
        }
        PublishFullInventory(router_, *inv_, *sessions_, pid, x, y, z);
      });
}

} // namespace simcore
