#include "ToolActionHandler.h"
#include "WrenchHandler.h"
#include "../ECS/components/ItemEnergyStorage.h"
#include "ECS/SimulationEngine.h"
#include "Storage/PlayerInventoryStore.h"
#include "Network/clients/IoUringRouterClient.h"
#include "MiningCalculator.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
ToolActionHandler::ToolActionHandler(std::shared_ptr<SimulationEngine> engine, std::shared_ptr<PlayerInventoryStore> inv, std::shared_ptr<IoUringRouterClient> r)
    : engine_(std::move(engine)), inventoryStore_(std::move(inv)), router_(std::move(r)) {}
void ToolActionHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::ToolAction>(nullptr)) return;
    auto* action = flatbuffers::GetRoot<Protocol::ToolAction>(data.data());
    if (!action || !action->pos()) return;
    auto* p = action->pos();
    flatbuffers::FlatBufferBuilder fbb(128); std::vector<uint8_t> respData;
    switch (action->action()) {
    case Protocol::ToolActionType_WRENCH_CYCLE: {
        WrenchHandler wh(engine_->reg());
        auto r = wh.cycleFace(action->player_id(), p->x(), p->y(), p->z(), action->face());
        auto err = r.error.empty() ? 0 : fbb.CreateString(r.error);
        auto roles = fbb.CreateVector(r.allRoles, 6);
        auto resp = Protocol::CreateToolActionResp(fbb, r.success, err, 0, 0, r.newRole, roles);
        fbb.Finish(resp); respData.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()); break;
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
    case Protocol::ToolActionType_CHARGE_ITEM: spdlog::info("[ToolAction] CHARGE_ITEM not yet implemented"); break;
    default: spdlog::warn("[ToolAction] unhandled type {}", static_cast<int>(action->action())); break;
    }
    if (!respData.empty()) router_->Publish("player.tool.action.response", std::move(respData));
}
} // namespace simcore
