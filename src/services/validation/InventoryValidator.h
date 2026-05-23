#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <protocol/core_generated.h>

/**
 * Inventory Validation Rules
 * 
 * Centralized validation for inventory operations to ensure data integrity
 * across all services. Follows the constraints defined in the specifications.
 */
class InventoryValidator {
public:
    /**
     * Validate ItemStack constraints
     * @param itemId Item/block ID
     * @param count Stack count
     * @param meta Item metadata/damage
     * @return Error message if validation fails, empty string if valid
     */
    static std::string ValidateItemStack(uint16_t itemId, uint8_t count, uint16_t meta) {
        // Validate item_id range: 0 (empty) or 1-65535 (valid items)
        if (itemId > 65535) {
            return "item_id must be <= 65535";
        }
        // Validate count range: 0-64 (0 = empty slot)
        if (count > 64) {
            return "count must be <= 64 (MVP limit)";
        }
        // Validate meta range: 0-65535
        if (meta > 65535) {
            return "meta must be <= 65535";
        }
        // Validate empty slot constraint: item_id=0 must have count=0
        if (itemId == 0 && count != 0) {
            return "empty slot must have count=0";
        }
        return ""; // Valid
    }

    /**
     * Validate InventorySlot constraints
     * @param slot InventorySlot to validate
     * @return Error message if validation fails, empty string if valid
     */
    static std::string ValidateInventorySlot(const Protocol::InventorySlot* slot) {
        if (!slot) {
            return "InventorySlot is null";
        }
        return ValidateItemStack(slot->item_id(), slot->count(), slot->meta());
    }

    /**
     * Validate Inventory constraints
     * @param inventory Inventory to validate
     * @return Error message if validation fails, empty string if valid
     */
    static std::string ValidateInventory(const Protocol::Inventory* inventory) {
        if (!inventory) {
            return "Inventory is null";
        }

        // Validate size field matches actual slot count
        if (inventory->slots()->size() != inventory->size()) {
            return "Inventory size field does not match actual slot count";
        }

        // Validate each slot in inventory
        for (int i = 0; i < inventory->slots()->size(); ++i) {
            auto slot = inventory->slots()->Get(i);
            std::string slotError = ValidateInventorySlot(slot);
            if (!slotError.empty()) {
                return "Invalid slot at index " + std::to_string(i) + ": " + slotError;
            }
        }

        return ""; // Valid
    }

    /**
     * Validate InventoryType constraints
     * @param inventoryType InventoryType to validate
     * @return Error message if validation fails, empty string if valid
     */
    static std::string ValidateInventoryType(const Protocol::InventoryType* inventoryType) {
        if (!inventoryType) {
            return "InventoryType is null";
        }

        // Validate slot count range
        if (inventoryType->slot_count() > 1000) {
            return "inventory slot count unreasonably large";
        }
        if (inventoryType->slot_count() == 0) {
            return "inventory slot count must be > 0";
        }

        // Validate inventory type enum (0=Player, 1=Machine, 2=Workbench, 3=Chest)
        uint8_t type = inventoryType->type();
        if (type > 3) {
            return "invalid inventory type (must be 0-3)";
        }

        // Validate specific slot counts per inventory type
        switch (type) {
            case 0: // Player inventory
                if (inventoryType->slot_count() != 45) {
                    return "Player inventory must have exactly 45 slots (36+9 hotbar)";
                }
                break;
            case 2: // Workbench inventory
                if (inventoryType->slot_count() != 10) {
                    return "Workbench inventory must have exactly 10 slots (9 crafting + 1 result)";
                }
                break;
            // Machine and Chest inventories have variable slot counts per definition
            default:
                break;
        }

        return ""; // Valid
    }

    /**
     * Validate EntityState constraints
     * @param entityState EntityState to validate
     * @return Error message if validation fails, empty string if valid
     */
    static std::string ValidateEntityState(const Protocol::EntityState* entityState) {
        if (!entityState) {
            return "EntityState is null";
        }

        // Validate EntityState contains valid Inventory data when not empty
        if (entityState->state()->size() > 0) {
            auto verifier = flatbuffers::Verifier(
                entityState->state()->data(), 
                entityState->state()->size()
            );
            if (!verifier.VerifyBuffer<Protocol::Inventory>(nullptr)) {
                return "EntityState state buffer does not contain valid Inventory data";
            }
            
            auto inventory = flatbuffers::GetRoot<Protocol::Inventory>(entityState->state()->data());
            std::string inventoryError = ValidateInventory(inventory);
            if (!inventoryError.empty()) {
                return "EntityState contains invalid Inventory: " + inventoryError;
            }
        }

        return ""; // Valid
    }
};