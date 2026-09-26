#include "ElectricDrillHandler.h"
#include "../MiningCalculator.h"
#include <game/machines/ItemEnergyStorage.h>
#include <apps/simcore/InventoryActionHandler.h>
#include <data/registry/ToolIds.h>
#include <spdlog/spdlog.h>

namespace simcore {

ElectricDrillHandler::ElectricDrillHandler(
    std::function<uint16_t(int32_t, int32_t, int32_t)> getBlock,
    std::function<void(int32_t, int32_t, int32_t, uint16_t)> setBlock,
    std::shared_ptr<PlayerInventoryStore> inventoryStore)
    : getBlock_(std::move(getBlock))
    , setBlock_(std::move(setBlock))
    , inventoryStore_(std::move(inventoryStore))
{}

bool ElectricDrillHandler::canMineBlock(uint16_t tool_item_id, uint16_t block_id) {
    uint8_t tier = toolTier(tool_item_id);
    uint8_t toolLevel = miningLevel(tier);
    uint8_t requiredLevel = getBlockMiningLevel(block_id);
    return toolLevel >= requiredLevel;
}

float ElectricDrillHandler::getMiningTicks(uint16_t tool_item_id, uint16_t block_id) {
    uint8_t tier = toolTier(tool_item_id);
    return miningTicks(tier, block_id);
}

DrillMineResult ElectricDrillHandler::mineBlock(
    uint64_t player_id,
    int32_t x, int32_t y, int32_t z,
    uint16_t tool_item_id,
    uint8_t slot_idx)
{
    DrillMineResult result;

    uint8_t tier = toolTier(tool_item_id);
    if (tier == 0 && tool_item_id != ITEM_DRILL_ULV) {
        result.error = "not_a_drill";
        return result;
    }

    uint16_t block_id = getBlock_(x, y, z);
    if (block_id == 0) {
        result.error = "block_is_air";
        return result;
    }

    if (!canMineBlock(tool_item_id, block_id)) {
        result.error = "tier_too_low";
        return result;
    }

    auto slots = inventoryStore_->getSlots(player_id);
    if (slot_idx >= slots.size()) {
        result.error = "invalid_slot";
        return result;
    }

    auto& toolSlot = slots[slot_idx];
    if (toolSlot.item_id != tool_item_id) {
        result.error = "slot_mismatch";
        return result;
    }

    int32_t energyCost = miningEnergyCost(tool_item_id, block_id);
    simulation_core::ItemStack toolStack{tool_item_id, 1, toolSlot.meta};

    // Fail closed, with the refusal reason split from the out-of-charge one.
    // All four shipped drill ids (items.csv:449-452, 1111:00:0..3) are absent
    // from TOOL_ENERGY_DEFS, and no content file declares a drill capacity —
    // items.csv has no energy column and machines.yaml has no drill rows — so
    // the table is incomplete rather than the drill path being wrong. Until it
    // is filled, such a tool has no energy model at all, which is a DIFFERENT
    // diagnosis from "your battery is flat": the latter is what drives the
    // client out-of-energy toast, so conflating them misreports the state.
    // Checked here, in the energy phase, ahead of the charge check.
    const auto def = TOOL_ENERGY_DEFS.find(tool_item_id);
    if (def == TOOL_ENERGY_DEFS.end()) {
        result.error = "no_energy_definition";
        spdlog::warn("[Drill] {} tool {} has no ToolEnergyDef; refusing to mine "
                     "(the energy table is incomplete for this item)",
                     player_id, tool_item_id);
        return result;
    }

    if (!consumeToolEnergy(toolStack, energyCost)) {
        result.error = "no_energy";
        return result;
    }

    setBlock_(x, y, z, 0);

    toolSlot.meta = toolStack.meta;
    inventoryStore_->setSlots(player_id, slots);

    int32_t remaining = getToolEnergy(toolStack);
    result.success = true;
    result.energy_remaining = remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
    result.mined_block_id = block_id;

    // find(), not at(): a capacity lookup must never be able to throw out of a
    // handler that no caller catches. This mirrors ToolActionHandler.cpp:34/50.
    // The `def` guard above has already proven the key exists, so this lookup
    // is redundant by construction — that redundancy is the point, and the
    // regression guard is the exhaustive no-id-throws test in
    // test_block_actions.cpp.
    spdlog::info("[Drill] {} mined block {} at ({},{},{}) energy={}/{}",
                 player_id, block_id, x, y, z,
                 toolStack.meta, def->second.capacity);
    return result;
}

} // namespace simcore
