#include "InventoryActionHandler.h"
#include "PlayerInventoryStore.h"
#include "Network/clients/IoUringRouterClient.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
InventoryActionHandler::InventoryActionHandler(std::shared_ptr<PlayerInventoryStore> inv, std::shared_ptr<IoUringRouterClient> r)
    : inventoryStore_(std::move(inv)), router_(std::move(r)) {}
void InventoryActionHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::InventoryAction>(nullptr)) return;
    auto* action = flatbuffers::GetRoot<Protocol::InventoryAction>(data.data());
    if (!action) return;
    uint64_t pid = action->player_id(); uint8_t at = action->action_type(), src = action->source_slot(), dst = action->target_slot();
    auto pub = [&]() { flatbuffers::FlatBufferBuilder fb(256);
        auto u = inventoryStore_->buildUpdate(fb, pid); fb.Finish(u);
        router_->Publish("player.inventory.update", {fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize()}); };
    switch (at) {
    case 0: { auto inv = inventoryStore_->getSlots(pid); if (src>=inv.size()||dst>=inv.size()) return; std::swap(inv[src],inv[dst]); inventoryStore_->setSlots(pid,inv); pub(); break; }
    case 1: { auto inv = inventoryStore_->getSlots(pid); if (src>=inv.size()||dst>=inv.size()||inv[src].item_id==0) return;
        uint8_t sc = action->count(); if (sc==0||sc>inv[src].count) sc=inv[src].count/2; if (sc==0) return;
        inv[dst]={inv[src].item_id,sc,inv[src].meta}; inv[src].count-=sc; if(inv[src].count==0) inv[src].item_id=0;
        inventoryStore_->setSlots(pid,inv); pub(); break; }
    case 2: { auto inv = inventoryStore_->getSlots(pid); if (src>=inv.size()) return; inv[src]={}; inventoryStore_->setSlots(pid,inv); pub(); break; }
    default: spdlog::warn("[SimCore] Unknown inv action type {} from {}", at, pid);
    }
}
} // namespace simcore
