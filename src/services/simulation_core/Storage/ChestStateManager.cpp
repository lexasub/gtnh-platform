#include "Storage/ChestStateManager.h"
#include "Storage/ContainerSession.h"
#include "Network/clients/IoUringRouterClient.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "core_generated.h"
#include "machine_state_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

ChestStateManager::ChestStateManager(
    std::shared_ptr<EntityStateStoreClient> essClient, int32_t dimension)
    : essClient_(std::move(essClient)), dimension_(dimension) {}

uint64_t ChestStateManager::posKey(int32_t x, int32_t y, int32_t z) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) ^
         (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 16) ^
         static_cast<uint64_t>(static_cast<uint32_t>(z));
}

void ChestStateManager::loadSlots(int32_t x, int32_t y, int32_t z,
                                  LoadCallback cb,
                                  uint16_t entity_type) {
  uint64_t key = posKey(x, y, z);
  auto it = cache_.find(key);
  if (it != cache_.end()) {
    cb(it->second);
    return;
  }
  if (!essClient_) {
    cb({});
    return;
  }
  essClient_->LoadEntityState(
      dimension_, x, y, z, entity_type,
      [this, key, cb](const EntityStateStoreClient::EntityStateData& state) {
        auto slots = DecodeChestBlob(state.state);
        cache_[key] = slots;
        cb(slots);
      });
}

void ChestStateManager::saveSlots(int32_t x, int32_t y, int32_t z,
                                  const std::vector<PersistSlot>& slots,
                                  uint16_t entity_type) {
  cache_[posKey(x, y, z)] = slots;
  if (!essClient_) return;
  auto blob = EncodeChestBlob(slots);
  essClient_->SaveEntityState(dimension_, x, y, z, entity_type, blob,
                              [x, y, z](bool ok) {
    spdlog::debug("[ChestStateManager] save at ({},{},{}) — {}", x, y, z,
                  ok ? "OK" : "FAIL");
  });
}

void ChestStateManager::clearSlots(int32_t x, int32_t y, int32_t z,
                                   uint16_t entity_type) {
  cache_.erase(posKey(x, y, z));
  if (essClient_) {
    essClient_->SaveEntityState(dimension_, x, y, z, entity_type, {},
                                [x, y, z](bool ok) {
      spdlog::debug("[ChestStateManager] clear at ({},{},{}) — {}", x, y, z,
                    ok ? "OK" : "FAIL");
    });
  }
}

// ── MachineState blob helpers ───────────────────────────────────────────────

std::vector<uint8_t> EncodeChestBlob(const std::vector<PersistSlot>& slots) {
  flatbuffers::FlatBufferBuilder fbb(256);
  std::vector<flatbuffers::Offset<Protocol::MachineInventorySlot>> offs;
  offs.reserve(slots.size());
  for (const auto& s : slots) {
    // MachineInventorySlot.count is uint16 in the schema; PersistSlot.count is
    // uint8 — widen on encode (clamp decode).
    offs.push_back(Protocol::CreateMachineInventorySlot(
        fbb, s.item_id, static_cast<uint16_t>(s.count), s.meta));
  }
  auto inv = Protocol::CreateMachineInventory(fbb, static_cast<uint8_t>(offs.size()),
                                              fbb.CreateVector(offs));
  auto st = Protocol::CreateMachineState(fbb, 1, 0, 0, inv, 0);
  fbb.Finish(st);
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

std::vector<PersistSlot> DecodeChestBlob(const std::vector<uint8_t>& blob) {
  std::vector<PersistSlot> slots;
  if (blob.empty()) return slots;
  flatbuffers::Verifier v(blob.data(), blob.size());
  if (!v.VerifyBuffer<Protocol::MachineState>(nullptr)) return slots;
  auto fbState = flatbuffers::GetRoot<Protocol::MachineState>(blob.data());
  auto* inv = fbState->inventory();
  if (!inv || !inv->slots()) return slots;
  slots.reserve(inv->slots()->size());
  for (flatbuffers::uoffset_t i = 0; i < inv->slots()->size(); ++i) {
    auto* s = inv->slots()->Get(i);
    if (!s) { slots.push_back({}); continue; }
    // Clamp count to uint8 (schema stores uint16).
    uint8_t cnt = s->count() > 255 ? 255 : static_cast<uint8_t>(s->count());
    slots.push_back({static_cast<uint16_t>(s->item_id()), cnt,
                     static_cast<uint16_t>(s->meta())});
  }
  return slots;
}

// ── Shared snapshot publisher ───────────────────────────────────────────────

void PublishFullInventory(std::shared_ptr<IoUringRouterClient> router,
                          PlayerInventoryStore& store,
                          ContainerSessionRegistry& sessions,
                          uint64_t pid, int32_t x, int32_t y, int32_t z) {
  if (!router) return;
  ContainerSession* sess = sessions.find(pid);
  if (!sess) return;
  PublishFullInventory(router, store, *sess, pid, x, y, z);
}

void PublishFullInventory(std::shared_ptr<IoUringRouterClient> router,
                          PlayerInventoryStore& store,
                          ContainerSession& sess,
                          uint64_t pid, int32_t x, int32_t y, int32_t z) {
  if (!router) return;
  flatbuffers::FlatBufferBuilder fb(1024);

  auto playerSlots = store.getSlots(pid);
  std::vector<flatbuffers::Offset<Protocol::InventorySlot>> fbSlots;
  fbSlots.reserve(kInventorySlots);
  for (const auto& s : playerSlots) {
    fbSlots.push_back(Protocol::CreateInventorySlot(fb, s.item_id, s.count, s.meta));
  }
  auto slotsVec = fb.CreateVector(fbSlots);

  auto cursor = store.getCursor(pid);
  Protocol::ItemStack cursorStruct(cursor.item_id, cursor.count, cursor.meta);

  std::vector<flatbuffers::Offset<Protocol::InventorySlot>> fbCont;
  if (auto* cont = sess.slotsRef()) {
    fbCont.reserve(cont->size());
    for (const auto& s : *cont) {
      fbCont.push_back(Protocol::CreateInventorySlot(fb, s.item_id, s.count, s.meta));
    }
  }
  auto contVec = fb.CreateVector(fbCont);

  Protocol::Vec3i pos(x, y, z);
  auto upd = Protocol::CreateInventoryUpdate(fb, pid, slotsVec, &cursorStruct,
                                             1 /*container_id*/, &pos, contVec);
  fb.Finish(upd);
  router->Publish("player.inventory.update",
                  {fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize()});
}

} // namespace simcore
