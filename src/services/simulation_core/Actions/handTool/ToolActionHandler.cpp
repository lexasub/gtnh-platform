#include "ToolActionHandler.h"
#include "../../ECS/components/ItemEnergyStorage.h"
#include "Quest/QuestManager.h"
#include "ECS/SimulationEngine.h"
#include "Storage/PlayerInventoryStore.h"
#include "Network/clients/IoUringRouterClient.h"
#include "../MiningCalculator.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
ToolActionHandler::ToolActionHandler(std::shared_ptr<SimulationEngine> engine, std::shared_ptr<PlayerInventoryStore> inv, std::shared_ptr<IoUringRouterClient> r, std::shared_ptr<QuestManager> qm)
    : engine_(std::move(engine)), inventoryStore_(std::move(inv)), router_(std::move(r)), questManager_(std::move(qm)) {}

void ToolActionHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::ToolAction>(nullptr)) return;
    auto* action = flatbuffers::GetRoot<Protocol::ToolAction>(data.data());
    if (!action || !action->pos()) return;
    auto* p = action->pos();
    flatbuffers::FlatBufferBuilder fbb(128); std::vector<uint8_t> respData;
    switch (action->action()) {
    case Protocol::ToolActionType_WRENCH_CYCLE: {
        break;
    }
    case Protocol::ToolActionType_MINE_BLOCK: {
        uint16_t toolId = action->item_id(); uint8_t slotIdx = action->slot_idx();
        auto slots = inventoryStore_->getSlots(action->player_id());
        bool hasDrill = false; uint32_t energyRemaining = 0;
        if (slotIdx < slots.size() && slots[slotIdx].item_id == toolId) {
            uint8_t tier = toolTier(toolId);
            if (tier > 0 || toolId == ITEM_DRILL_ULV) {
                hasDrill = true;
                simulation_core::ItemStack ts{toolId, 1, slots[slotIdx].meta};
                auto it = TOOL_ENERGY_DEFS.find(toolId);
                if (it != TOOL_ENERGY_DEFS.end()) energyRemaining = static_cast<uint32_t>(getToolEnergy(ts));
            }
        }
        auto resp = Protocol::CreateToolActionResp(fbb, hasDrill, 0, energyRemaining, 0, 0, 0);
        fbb.Finish(resp); respData.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()); break;
    }
    case Protocol::ToolActionType_CHARGE_ITEM: {
        // TOOL_CHARGED detection: when the charged tool is at full capacity,
        // forward the tool id to QuestManager::checkToolCharged().
        uint16_t toolId = action->item_id();
        uint8_t slotIdx = action->slot_idx();
        auto slots = inventoryStore_->getSlots(action->player_id());
        int32_t energy = -1, capacity = -1;
        if (slotIdx < slots.size() && slots[slotIdx].item_id == toolId) {
            simulation_core::ItemStack ts{toolId, 1, slots[slotIdx].meta};
            auto it = TOOL_ENERGY_DEFS.find(toolId);
            if (it != TOOL_ENERGY_DEFS.end()) {
                energy = getToolEnergy(ts);
                capacity = it->second.capacity;
            }
        }
        const bool fullyCharged = (energy >= 0) && (energy >= capacity);
        if (questManager_ && fullyCharged) {
            questManager_->checkToolCharged(action->player_id(), toolId);
        }
        spdlog::info("[ToolAction] CHARGE_ITEM player={} tool={} slot={} energy={} capacity={} fully_charged={}",
                     action->player_id(), toolId, slotIdx, energy, capacity, fullyCharged);
        break;
    }
    default: spdlog::warn("[ToolAction] unhandled type {}", static_cast<int>(action->action())); break;
    }
    if (!respData.empty()) router_->Publish("player.tool.action.response", std::move(respData));
}
} // namespace simcore
