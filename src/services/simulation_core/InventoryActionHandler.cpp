#include "InventoryActionHandler.h"
#include "Network/clients/MessageRouterClient.h"
#include "core_generated.h"
#include "meta_db_generated.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdint>

namespace simulation_core {
//TODO rewrite to Iouring router
InventoryActionHandler::InventoryActionHandler(std::shared_ptr<simcore::MessageRouterClient> router)
    : router_(std::move(router))
{
    spdlog::debug("[InventoryActionHandler] Created");
}

InventoryActionHandler::~InventoryActionHandler() {
    spdlog::debug("[InventoryActionHandler] Destroyed");
}

void InventoryActionHandler::handleAction(const InventoryActionRequest& request) {
    if (!validateRequest(request)) {
        spdlog::warn("[InventoryActionHandler] Invalid request: player={} action={} slot={}",
                     request.player_id, static_cast<int>(request.action_type), request.slot_id);
        return;
    }
    switch (request.action_type) {
        case InventoryActionType::PICKUP_ITEM:    onItemPicked(request); break;
        case InventoryActionType::DROP_ITEM:      onItemDropped(request); break;
        case InventoryActionType::TAKE_ITEM:      onItemTaken(request); break;
        case InventoryActionType::PLACE_ITEM:     onItemPlaced(request); break;
        case InventoryActionType::CRAFT_ITEM:     onItemCrafted(request); break;
        case InventoryActionType::USE_ITEM:       onItemUsed(request); break;
        case InventoryActionType::BREAK_ITEM:     onItemBroken(request); break;
        case InventoryActionType::INSERT_ITEM:    onItemInserted(request); break;
        case InventoryActionType::REMOVE_ITEM:    onItemRemoved(request); break;
        case InventoryActionType::HOTBAR_CHANGE:  onHotbarChanged(request); break;
        case InventoryActionType::INVENTORY_OPEN: onInventoryOpened(request); break;
        case InventoryActionType::INVENTORY_CLOSE: onInventoryClosed(request); break;
        case InventoryActionType::CRAFTING_OPEN:  onCraftingOpened(request); break;
        case InventoryActionType::CRAFTING_CLOSE: onCraftingClosed(request); break;
        default:
            spdlog::warn("[InventoryActionHandler] Unknown action type: {}",
                         static_cast<int>(request.action_type));
            break;
    }
}

InventoryState InventoryActionHandler::getPlayerInventory(uint64_t player_id) const {
    auto it = inventories_.find(player_id);
    return (it != inventories_.end()) ? it->second : InventoryState{player_id, {}};
}

InventorySlot InventoryActionHandler::getItemAtSlot(uint64_t player_id, int16_t slot_index) const {
    auto it = inventories_.find(player_id);
    if (it == inventories_.end()) return InventorySlot{0, 0, 0, slot_index};
    for (const auto& slot : it->second.slots) {
        if (slot.slot_index == slot_index) return slot;
    }
    return InventorySlot{0, 0, 0, slot_index};
}

void InventoryActionHandler::setItemAtSlot(uint64_t player_id, int16_t slot_index, const ItemStack& item) {
    auto& inv = inventories_[player_id];
    inv.player_id = player_id;
    for (auto& slot : inv.slots) {
        if (slot.slot_index == slot_index) {
            slot.item_id = item.item_id;
            slot.count = item.count;
            slot.meta = item.meta;
            return;
        }
    }
    inv.slots.push_back({item.item_id, item.count, item.meta, slot_index});
}

int16_t InventoryActionHandler::getSlotCount() const {
    return MAX_SLOTS;
}

static void addItemToInventory(std::vector<InventorySlot>& slots, uint16_t item_id,
                                uint8_t count, uint16_t meta, int16_t max_slots) {
    if (item_id == 0 || count == 0) return;
    for (auto& slot : slots) {
        if (slot.item_id == item_id && slot.count < 64) {
            uint8_t space = 64 - slot.count;
            uint8_t added = std::min(space, count);
            slot.count += added;
            count -= added;
            if (count == 0) return;
        }
    }
    for (int16_t i = 0; i < max_slots; ++i) {
        bool occupied = false;
        for (const auto& slot : slots) {
            if (slot.slot_index == i) { occupied = true; break; }
        }
        if (!occupied) {
            slots.push_back({item_id, count, meta, i});
            return;
        }
    }
}

static bool removeFromSlot(std::vector<InventorySlot>& slots, int16_t slot_index, uint8_t count) {
    auto it = std::find_if(slots.begin(), slots.end(),
        [slot_index](const InventorySlot& s) { return s.slot_index == slot_index; });
    if (it == slots.end() || it->item_id == 0) return false;
    if (count >= it->count) slots.erase(it);
    else it->count -= count;
    return true;
}

void InventoryActionHandler::onItemPicked(const InventoryActionRequest& request) {
    auto& inv = inventories_[request.player_id];
    inv.player_id = request.player_id;
    addItemToInventory(inv.slots, request.item.item_id, request.item.count,
                       request.item.meta, MAX_SLOTS);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemDropped(const InventoryActionRequest& request) {
    auto& inv = inventories_[request.player_id];
    inv.player_id = request.player_id;
    uint8_t qty = request.item.count;
    if (qty == 0) qty = 1;
    if (!removeFromSlot(inv.slots, request.slot_id, qty)) {
        spdlog::warn("[InventoryActionHandler] onItemDropped: slot {} empty/missing (player={})",
                     request.slot_id, request.player_id);
        return;
    }
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemTaken(const InventoryActionRequest& request) {
    auto& inv = inventories_[request.player_id];
    inv.player_id = request.player_id;
    addItemToInventory(inv.slots, request.item.item_id, request.item.count,
                       request.item.meta, MAX_SLOTS);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemPlaced(const InventoryActionRequest& request) {
    auto& inv = inventories_[request.player_id];
    inv.player_id = request.player_id;
    uint8_t qty = request.item.count;
    if (qty == 0) qty = 1;
    if (!removeFromSlot(inv.slots, request.slot_id, qty)) {
        spdlog::warn("[InventoryActionHandler] onItemPlaced: slot {} empty/missing (player={})",
                     request.slot_id, request.player_id);
        return;
    }
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemCrafted(const InventoryActionRequest& request) {
    auto& inv = inventories_[request.player_id];
    inv.player_id = request.player_id;
    if (request.slot_id >= 0) {
        removeFromSlot(inv.slots, request.slot_id, 1);
    }
    addItemToInventory(inv.slots, request.item.item_id, request.item.count,
                       request.item.meta, MAX_SLOTS);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemUsed(const InventoryActionRequest& request) {
    spdlog::info("[InventoryActionHandler] onItemUsed: player={} item={} slot={}",
                 request.player_id, request.item.item_id, request.slot_id);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemBroken(const InventoryActionRequest& request) {
    spdlog::info("[InventoryActionHandler] onItemBroken: player={} item={} slot={}",
                 request.player_id, request.item.item_id, request.slot_id);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemInserted(const InventoryActionRequest& request) {
    auto& inv = inventories_[request.player_id];
    inv.player_id = request.player_id;
    addItemToInventory(inv.slots, request.item.item_id, request.item.count,
                       request.item.meta, MAX_SLOTS);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onItemRemoved(const InventoryActionRequest& request) {
    auto& inv = inventories_[request.player_id];
    inv.player_id = request.player_id;
    uint8_t qty = request.item.count;
    if (qty == 0) qty = 1;
    if (!removeFromSlot(inv.slots, request.slot_id, qty)) {
        spdlog::warn("[InventoryActionHandler] onItemRemoved: slot {} empty/missing (player={})",
                     request.slot_id, request.player_id);
        return;
    }
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onHotbarChanged(const InventoryActionRequest& request) {
    spdlog::info("[InventoryActionHandler] onHotbarChanged: player={} slot={}",
                 request.player_id, request.slot_id);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onInventoryOpened(const InventoryActionRequest& request) {
    spdlog::info("[InventoryActionHandler] onInventoryOpened: player={}", request.player_id);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onInventoryClosed(const InventoryActionRequest& request) {
    spdlog::info("[InventoryActionHandler] onInventoryClosed: player={}", request.player_id);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onCraftingOpened(const InventoryActionRequest& request) {
    spdlog::info("[InventoryActionHandler] onCraftingOpened: player={}", request.player_id);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::onCraftingClosed(const InventoryActionRequest& request) {
    spdlog::info("[InventoryActionHandler] onCraftingClosed: player={}", request.player_id);
    notifyMessageRouter(request);
    notifyMetaDB(request);
}

void InventoryActionHandler::notifyMessageRouter(const InventoryActionRequest& request) {
    if (!router_) return;

    flatbuffers::FlatBufferBuilder builder(64);
    auto action = Protocol::CreateInventoryAction(
        builder,
        request.player_id,
        static_cast<uint8_t>(request.action_type),
        static_cast<uint8_t>(request.slot_id),
        0xFF,
        request.item.count
    );
    builder.Finish(action);
    std::vector<uint8_t> data(builder.GetBufferPointer(),
                              builder.GetBufferPointer() + builder.GetSize());
    router_->Publish("player.inventory.actions", data);
    spdlog::debug("[InventoryActionHandler] Published action={} player={}",
                  static_cast<int>(request.action_type), request.player_id);
}

void InventoryActionHandler::notifyMetaDB(const InventoryActionRequest& request) {
    if (!router_) return;

    flatbuffers::FlatBufferBuilder builder(128);
    auto req = Protocol::CreateSetInventorySlotReq(
        builder,
        request.player_id,
        static_cast<uint16_t>(request.slot_id),
        request.item.item_id,
        request.item.count,
        request.item.meta
    );

    auto msg = Protocol::CreateMetaDBMessage(
        builder,
        0,
        Protocol::MetaDBRequest_SetInventorySlotReq,
        req.Union()
    );

    auto frame = Protocol::CreateMetaDBFrame(
        builder,
        Protocol::MetaDBPayload_MetaDBMessage,
        msg.Union()
    );
    builder.Finish(frame);
    std::vector<uint8_t> data(builder.GetBufferPointer(),
                              builder.GetBufferPointer() + builder.GetSize());
    router_->Publish("meta_db.inventory.set", data);
    spdlog::debug("[InventoryActionHandler] Published to meta_db.inventory.set: player={} slot={} item={}",
                  request.player_id, request.slot_id, request.item.item_id);
}

bool InventoryActionHandler::validateRequest(const InventoryActionRequest& request) {
    if (request.player_id == 0) {
        spdlog::warn("[InventoryActionHandler] validate: player_id is 0");
        return false;
    }
    if (request.slot_id < -1 || request.slot_id >= MAX_SLOTS) {
        spdlog::warn("[InventoryActionHandler] validate: slot_id {} out of range [-1, {}]",
                     request.slot_id, MAX_SLOTS - 1);
        return false;
    }
    return true;
}

}
