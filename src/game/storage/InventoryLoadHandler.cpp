#include "InventoryLoadHandler.h"
#include "PlayerInventoryStore.h"
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
InventoryLoadHandler::InventoryLoadHandler(std::shared_ptr<PlayerInventoryStore> inv, std::shared_ptr<IoUringRouterClient> r)
    : inventoryStore_(std::move(inv)), router_(std::move(r)) {}
void InventoryLoadHandler::handle(const std::vector<uint8_t>& data) {
    // gp-ajvg: "player.inventory.load" is a router-subscribed topic, so ANY
    // publisher on the bus can deliver a zero-length or truncated payload.
    // For an empty std::vector, data.data() is nullptr, so the Verifier-less
    // GetRoot() below returned a Table whose first accessor dereferenced
    // null — a crash that takes the whole SimCore daemon down (confirmed by
    // gdb: ReadScalar<int>(p=0x0) <- Table::GetVTable(this=0x0) <-
    // PlayerJoinedHandler::handle). A garbage-but-nonempty buffer is just as
    // bad: it would make applyUpdate() write a fabricated save over the
    // player's real inventory. This is the same R1/R2 rejection
    // InventoryActionHandler.cpp:31-34 already performs, so the Verifier runs
    // BEFORE the buffer pointer is ever read.
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::InventoryUpdate>(nullptr)) {
        spdlog::warn("[SimCore] InventoryLoad: invalid InventoryUpdate buffer ({} bytes) — dropped",
                     data.size());
        return;
    }
    auto update = flatbuffers::GetRoot<Protocol::InventoryUpdate>(data.data());
    if (!update) return;
    uint64_t pid = update->player_id(); auto* slots = update->slots();
    // slots is optional in the schema, so a verified buffer may still carry
    // none: that is an empty save, not a crash.
    std::vector<PersistSlot> parsed; parsed.reserve(slots ? slots->size() : 0);
    for (uint16_t i = 0; slots && i < slots->size(); ++i) {
        auto* s = slots->Get(i); parsed.push_back({s->item_id(), static_cast<uint8_t>(s->count()), s->meta()});
    }
    inventoryStore_->applyUpdate(pid, parsed);
    spdlog::info("[SimCore] Loaded inventory for player {} ({} slots)", pid, parsed.size());
    flatbuffers::FlatBufferBuilder fb(512);
    auto fbUpdate = inventoryStore_->buildUpdate(fb, pid); fb.Finish(fbUpdate);
    router_->Publish("player.inventory.update", {fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize()});
}
} // namespace simcore
