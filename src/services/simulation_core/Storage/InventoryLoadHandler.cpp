#include "InventoryLoadHandler.h"
#include "PlayerInventoryStore.h"
#include "Network/clients/IoUringRouterClient.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
InventoryLoadHandler::InventoryLoadHandler(std::shared_ptr<PlayerInventoryStore> inv, std::shared_ptr<IoUringRouterClient> r)
    : inventoryStore_(std::move(inv)), router_(std::move(r)) {}
void InventoryLoadHandler::handle(const std::vector<uint8_t>& data) {
    auto update = flatbuffers::GetRoot<Protocol::InventoryUpdate>(data.data());
    uint64_t pid = update->player_id(); auto* slots = update->slots();
    std::vector<PersistSlot> parsed; parsed.reserve(slots->size());
    for (uint16_t i = 0; i < slots->size(); ++i) {
        auto* s = slots->Get(i); parsed.push_back({s->item_id(), static_cast<uint8_t>(s->count()), s->meta()});
    }
    inventoryStore_->applyUpdate(pid, parsed);
    spdlog::info("[SimCore] Loaded inventory for player {} ({} slots)", pid, parsed.size());
    flatbuffers::FlatBufferBuilder fb(512);
    auto fbUpdate = inventoryStore_->buildUpdate(fb, pid); fb.Finish(fbUpdate);
    router_->Publish("player.inventory.update", {fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize()});
}
} // namespace simcore
