#include "Network/MachineOpenHandler.h"
#include "Common/MainThreadQueue.h"
#include "ECS/SimulationEngine.h"
#include "ECS/components/Position.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/InventoryContainer.h"
#include "Network/clients/IoUringRouterClient.h"
#include "Storage/ChestStateManager.h"
#include "Storage/ContainerSession.h"
#include "Storage/ChunkStoreRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

MachineOpenHandler::MachineOpenHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                                       std::shared_ptr<ChestStateManager> stateMgr,
                                       std::shared_ptr<PlayerInventoryStore> inv,
                                       std::shared_ptr<IoUringRouterClient> router,
                                       std::shared_ptr<SimulationEngine> engine,
                                       std::shared_ptr<ChunkStoreRepository> chunkStore,
                                       MainThreadQueue* mainQueue)
    : sessions_(std::move(sessions))
    , stateMgr_(std::move(stateMgr))
    , inv_(std::move(inv))
    , router_(std::move(router))
    , engine_(std::move(engine))
    , chunkStore_(std::move(chunkStore))
    , mainQueue_(mainQueue) {}

void MachineOpenHandler::handle(const std::vector<uint8_t>& data) {
  flatbuffers::Verifier v(data.data(), data.size());
  if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) return;
  auto* req = flatbuffers::GetRoot<Protocol::ContainerOpenReq>(data.data());
  if (!req || !req->pos()) return;
  uint64_t pid = req->player_id();
  auto* p = req->pos();
  if (pid == 0) return;
  int32_t x = p->x(), y = p->y(), z = p->z();

  spdlog::info("[MachineOpen] player={} open machine at ({},{},{})", pid, x, y, z);

  auto& reg = engine_->reg();
  entt::entity entity = entt::null;
  auto vw = reg.view<const Position>();
  for (auto e : vw) {
    auto& pp = vw.get<const Position>(e);
    if (static_cast<int32_t>(pp.x) == x && static_cast<int32_t>(pp.y) == y &&
        static_cast<int32_t>(pp.z) == z) {
      entity = e;
      break;
    }
  }
  if (entity == entt::null) {
    spdlog::warn("[MachineOpen] no ECS entity at ({},{},{}) — lazy-init from ChunkStore", x, y, z);
    if (chunkStore_) {
      chunkStore_->getBlock(x, y, z,
          [engine = engine_, rx = x, ry = y, rz = z](const BlockData& bd) {
            if (bd.block_id != 0) {
              engine->onBlockChanged(rx, ry, rz, bd.block_id, bd.meta, bd.mb_id);
              spdlog::info("[SimCore] Lazy-created ECS entity at ({},{},{}) block_id={}",
                           rx, ry, rz, bd.block_id);
            }
          });
    }
    return; // retry on next open after lazy-init lands
  }

  auto* mc = reg.try_get<MachineComponent>(entity);
  if (!mc) {
    spdlog::warn("[MachineOpen] entity at ({},{},{}) is not a machine", x, y, z);
    return;
  }
  uint16_t machine_id = mc->machine_id;

  // Register the session IMMEDIATELY on the main thread. Slot count comes
  // from the ECS InventoryContainer (set up in onBlockChanged). The slots
  // start empty; loadSlots fills them from persisted data when the ESS
  // callback fires. Without pre-sizing, an empty vector (size 0) makes
  // SlotAt() return nullptr for every click index (slot >= 0 always true).
  auto* container = reg.try_get<InventoryContainer>(entity);
  size_t slotCount = container ? container->slots.size() : 0;

  ContainerSession sess;
  sess.kind = ContainerSession::Kind::Machine;
  sess.x = x; sess.y = y; sess.z = z;
  sess.entity_type = machine_id;
  sess.slots.assign(slotCount, PersistSlot{});
  sessions_->open(pid, sess);
  PublishFullInventory(router_, *inv_, *sessions_, pid, x, y, z);

  // Async load slots → fill owned copy → ECS-link on main queue.
  stateMgr_->loadSlots(x, y, z,
      [this, pid, x, y, z, entity, machine_id](const std::vector<PersistSlot>& slots) {
        ContainerSession* s = sessions_->find(pid);
        if (s && !slots.empty()) {
          size_t n = std::min(slots.size(), s->slots.size());
          for (size_t i = 0; i < n; ++i) s->slots[i] = slots[i];
        }
        PublishFullInventory(router_, *inv_, *sessions_, pid, x, y, z);

        if (!mainQueue_) return;
        mainQueue_->push([this, pid, x, y, z, entity] {
          auto& reg = engine_->reg();
          auto* c = reg.try_get<InventoryContainer>(entity);
          ContainerSession* s = sessions_->find(pid);
          if (!c || !s) return;
          // Merge clicks that landed before ECS link, then switch to live-ECS.
          c->slots.clear();
          c->slots.reserve(s->slots.size());
          for (auto& ps : s->slots)
            c->slots.push_back({ps.item_id, ps.count, ps.meta});
          s->reg = &reg;
          s->machine_entity = entity;
        });
      },
      machine_id);
}

} // namespace simcore
