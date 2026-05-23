// InventoryActionHandler.h
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
namespace simcore {
    class MessageRouterClient;
}

namespace simulation_core {

struct Vec3i {
    int32_t x, y, z;
    Vec3i() : x(0), y(0), z(0) {}
    Vec3i(int32_t _x, int32_t _y, int32_t _z) : x(_x), y(_y), z(_z) {}
};

// Represents an inventory item with item ID, count, and metadata.
// Matches FlatBuffers Protocol::ItemStack for binary compatibility.
struct ItemStack {
    uint16_t item_id = 0;   // Item type (0 = empty)
    uint8_t  count   = 0;   // Stack size (max 64)
    uint16_t meta    = 0;   // Damage/metadata (0 = new/untouched)
};

// Represents a single inventory slot.
// ItemStack fields + slot_index for position tracking.
// Matches FlatBuffers Protocol::InventorySlot for binary compatibility.
struct InventorySlot {
    uint16_t item_id    = 0;  // Item type (0 = empty)
    uint8_t  count      = 0;  // Stack size
    uint16_t meta       = 0;  // Damage/metadata
    int16_t  slot_index = 0;  // Which slot in the inventory
};

// Inventory state for a specific player
struct InventoryState {
    uint64_t player_id;
    std::vector<InventorySlot> slots;  // 36 slots (hotbar 9 + main 27)
};

// Action type for inventory operations
enum class InventoryActionType {
    PICKUP_ITEM,          // Player picked up an item
    DROP_ITEM,            // Player dropped an item
    TAKE_ITEM,            // Player took item from container
    PLACE_ITEM,           // Player placed item into container
    CRAFT_ITEM,           // Player crafted an item
    USE_ITEM,             // Player used an item
    BREAK_ITEM,           // Item was broken
    INSERT_ITEM,          // Item inserted from outside
    REMOVE_ITEM,          // Item removed from outside
    HOTBAR_CHANGE,        // Player changed hotbar slot
    INVENTORY_OPEN,       // Inventory GUI opened
    INVENTORY_CLOSE,      // Inventory GUI closed
    CRAFTING_OPEN,        // Crafting GUI opened
    CRAFTING_CLOSE,       // Crafting GUI closed
};

// Inventory action request
struct InventoryActionRequest {
    uint64_t player_id;
    InventoryActionType action_type;
    int16_t slot_id;      // Which slot is affected (-1 for global)
    ItemStack item;
    Vec3i position;      // Where it dropped from, if applicable
};

// Inventory action response
struct InventoryActionResponse {
    bool success;
    std::string error_message;
    uint64_t affected_player_id;  // Which player was affected
};

// Handles inventory-related actions from the simulation
class InventoryActionHandler {
public:
    explicit InventoryActionHandler(std::shared_ptr<simcore::MessageRouterClient> router);
    ~InventoryActionHandler();

    // Called when an inventory action occurs
    void handleAction(const InventoryActionRequest& request);

    // Called when an item is picked up
    void onItemPicked(const InventoryActionRequest& request);

    // Called when an item is dropped
    void onItemDropped(const InventoryActionRequest& request);

    // Called when an item is taken from a container
    void onItemTaken(const InventoryActionRequest& request);

    // Called when an item is placed into a container
    void onItemPlaced(const InventoryActionRequest& request);

    // Called when an item is crafted
    void onItemCrafted(const InventoryActionRequest& request);

    // Called when an item is used
    void onItemUsed(const InventoryActionRequest& request);

    // Called when an item is broken
    void onItemBroken(const InventoryActionRequest& request);

    // Called when an item is inserted from outside
    void onItemInserted(const InventoryActionRequest& request);

    // Called when an item is removed from outside
    void onItemRemoved(const InventoryActionRequest& request);

    // Called when the player changes their hotbar
    void onHotbarChanged(const InventoryActionRequest& request);

    // Called when the inventory GUI is opened
    void onInventoryOpened(const InventoryActionRequest& request);

    // Called when the inventory GUI is closed
    void onInventoryClosed(const InventoryActionRequest& request);

    // Called when the crafting GUI is opened
    void onCraftingOpened(const InventoryActionRequest& request);

    // Called when the crafting GUI is closed
    void onCraftingClosed(const InventoryActionRequest& request);

    // Get the current inventory state for a player
    InventoryState getPlayerInventory(uint64_t player_id) const;

    // Get an item at a specific slot
    InventorySlot getItemAtSlot(uint64_t player_id, int16_t slot_index) const;

    // Set an item at a specific slot
    void setItemAtSlot(uint64_t player_id, int16_t slot_index, const ItemStack& item);

    // Get the total number of slots in the inventory
    int16_t getSlotCount() const;

private:
    // Map of player_id -> inventory state
    std::unordered_map<uint64_t, InventoryState> inventories_;

    // Maximum number of slots (36: 9 hotbar + 27 main)
    static constexpr int16_t MAX_SLOTS = 36;

    // MessageRouter client for publishing events
    std::shared_ptr<simcore::MessageRouterClient> router_;

    // Called after any action to update MessageRouter
    void notifyMessageRouter(const InventoryActionRequest& request);

    // Called after any action to update MetaDB
    void notifyMetaDB(const InventoryActionRequest& request);

    // Validates an inventory action request
    bool validateRequest(const InventoryActionRequest& request);
};

} // namespace simulation_core
