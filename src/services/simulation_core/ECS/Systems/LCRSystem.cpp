#include "LCRSystem.h"
#include "../components/MachineComponent.h"
#include "../components/EnergyStorage.h"
#include "../components/InventoryContainer.h"
#include "../components/RecipeProgress.h"
#include "../components/Position.h"
#include "../components/MultiblockController.h"
#include "../Network/IEventPublisher.h"
#include "../Network/PipeEnergyClient.h"
#include "../RecipeManager/RecipeManager.h"
#include <recipe_manager_lib/RecipeTypes.h>
#include <spdlog/spdlog.h>

namespace simcore {

LCRSystem::LCRSystem(entt::registry& reg,
                     std::unordered_map<uint64_t, MultiblockController>& controllers,
                     const PatternRegistry& patterns,
                     std::shared_ptr<RecipeManager::RecipeManager> recipes,
                     std::shared_ptr<IEventPublisher> events,
                     std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), controllers_(controllers), patterns_(patterns),
      recipes_(recipes), events_(events), pipeClient_(pipeClient)
{}

void LCRSystem::tick(float) {
    std::vector<uint64_t> ids;
    for (const auto& [id, _] : controllers_) ids.push_back(id);

    for (uint64_t ctrl_id : ids) {
        auto it = controllers_.find(ctrl_id);
        if (it == controllers_.end()) continue;
        if (it->second.id == 0) continue;
        auto* pattern = patterns_.getPattern(3);
        if (!pattern) continue;
        tickLCR(ctrl_id, it->second);
    }
}

void LCRSystem::tickLCR(uint64_t ctrl_id, MultiblockController& ctrl) {
    (void)ctrl_id;
    auto view = reg_.view<const Position, MachineComponent>();
    entt::entity entity = entt::null;
    for (auto e : view) {
        auto& pos = view.get<const Position>(e);
        if (pos.x == ctrl.x && pos.y == ctrl.y && pos.z == ctrl.z) {
            entity = e;
            break;
        }
    }
    if (entity == entt::null) return;

    auto& machine = reg_.get<MachineComponent>(entity);
    auto& energy = reg_.get<EnergyStorage>(entity);
    auto& container = reg_.get<InventoryContainer>(entity);
    auto& progress = reg_.get<RecipeProgress>(entity);

    if (!progress.recipe_id.empty()) {
        auto* recipe = recipes_->getRecipeById(progress.recipe_id);
        if (!recipe) {
            progress.recipe_id.clear();
            progress.is_processing = false;
            return;
        }

        if (energy.current < static_cast<int32_t>(recipe->energy_cost)) {
            if (pipeClient_) {
                pipeClient_->sendConsumeRequest(
                    static_cast<uint64_t>(entity),
                    static_cast<int32_t>(machine.x),
                    static_cast<int32_t>(machine.y),
                    static_cast<int32_t>(machine.z),
                    static_cast<int32_t>(energy.type),
                    static_cast<int32_t>(recipe->energy_cost));
            }
            return;
        }

        energy.current -= static_cast<int32_t>(recipe->energy_cost);
        progress.remaining_ticks--;

        if (pipeClient_) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(entity),
                static_cast<int32_t>(machine.x),
                static_cast<int32_t>(machine.y),
                static_cast<int32_t>(machine.z),
                energy.current,
                energy.capacity,
                energy.maxInput,
                energy.maxOutput,
                energy.tier,
                static_cast<int32_t>(energy.type),
                false,
                true);
        }

        if (progress.remaining_ticks == 0) {
            for (const auto& out : recipe->outputs) {
                uint8_t remaining = out.count;
                for (auto& slot : container.slots) {
                    if (remaining == 0) break;
                    if (slot.item_id == out.item_id && slot.meta == out.metadata && slot.count < 64) {
                        uint8_t space = 64 - slot.count;
                        uint8_t add = std::min(remaining, space);
                        slot.count += add;
                        remaining -= add;
                    }
                }
                for (auto& slot : container.slots) {
                    if (remaining == 0) break;
                    if (slot.item_id == 0) {
                        slot = {out.item_id, remaining, out.metadata};
                        remaining = 0;
                    }
                }
            }

            progress.is_processing = false;
            progress.needs_output = true;
            progress.recipe_id.clear();
        }
    } else {
        std::vector<RecipeManager::ItemStack> inputItems;
        int slots_in = 0;
        if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id)) {
            slots_in = minfo->slots_in;
        }
        int input_end = std::min(slots_in, static_cast<int>(container.slots.size()));
        for (int i = 0; i < input_end; ++i) {
            auto& slot = container.slots[i];
            if (slot.item_id != 0) {
                inputItems.push_back({slot.item_id, slot.count, slot.meta});
            }
        }

        auto* recipe = recipes_->findRecipeByInputs(machine.machine_id, inputItems);
        if (recipe) {
            progress.recipe_id = recipe->id;
            progress.remaining_ticks = static_cast<int32_t>(recipe->duration);
            progress.is_processing = true;

            for (const auto& req : recipe->inputs) {
                if (req.item_id == 0) continue;
                int64_t remaining = static_cast<int64_t>(req.count);
                for (int i = 0; i < input_end && remaining > 0; ++i) {
                    auto& slot = container.slots[i];
                    if (slot.item_id == req.item_id && slot.meta == req.metadata) {
                        uint8_t take = std::min(slot.count, static_cast<uint8_t>(remaining));
                        slot.count -= take;
                        remaining -= take;
                        if (slot.count == 0) {
                            slot.item_id = 0;
                            slot.meta = 0;
                        }
                    }
                }
            }
        }
    }

    float pct = 0.0f;
    if (!progress.recipe_id.empty()) {
        auto* recipe = recipes_->getRecipeById(progress.recipe_id);
        if (recipe && recipe->duration > 0) {
            pct = 1.0f - static_cast<float>(progress.remaining_ticks) / static_cast<float>(recipe->duration);
        }
    }
    std::vector<uint8_t> inv_data;
    for (const auto& slot : container.slots) {
        inv_data.push_back(static_cast<uint8_t>(slot.item_id & 0xFF));
        inv_data.push_back(static_cast<uint8_t>((slot.item_id >> 8) & 0xFF));
        inv_data.push_back(slot.count);
        inv_data.push_back(static_cast<uint8_t>(slot.meta & 0xFF));
        inv_data.push_back(static_cast<uint8_t>((slot.meta >> 8) & 0xFF));
    }
    events_->publishBlockEntityUpdate(
        static_cast<int32_t>(machine.x),
        static_cast<int32_t>(machine.y),
        static_cast<int32_t>(machine.z),
        machine.machine_id,
        inv_data,
        pct,
        static_cast<uint32_t>(energy.current),
        energy.type,
        static_cast<uint32_t>(energy.capacity),
        0);
}

} // namespace simcore
