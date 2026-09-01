#include "LCRSystem.h"
#include "../components/MachineComponent.h"
#include "../components/EnergyStorage.h"
#include "../components/InventoryContainer.h"
#include "../components/RecipeProgress.h"
#include "../components/Position.h"
#include "../components/MultiblockController.h"
#include "../Network/IEventPublisher.h"
#include "../Network/PipeEnergyClient.h"
#include "../Network/CraftReservationClient.h"
#include "../SimulationEngine.h"
#include "../MultiblockUtils.h"
#include "../RecipeManager/RecipeManager.h"
#include <recipe_manager_lib/RecipeTypes.h>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace simcore {

LCRSystem::LCRSystem(entt::registry& reg,
                     std::unordered_map<uint64_t, MultiblockController>& controllers,
                     const PatternRegistry& patterns,
                     std::shared_ptr<RecipeManager::RecipeManager> recipes,
                     std::shared_ptr<IEventPublisher> events,
                     std::shared_ptr<PipeEnergyClient> pipeClient,
                     std::shared_ptr<CraftReservationClient> reservations)
    : reg_(reg), controllers_(controllers), patterns_(patterns),
      recipes_(recipes), events_(events), pipeClient_(pipeClient),
      reservations_(std::move(reservations))
{}

void LCRSystem::tick(float) {
    ++totalTicks_;
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

    // Item IO flows through ITEM_IN/ITEM_OUT hatch slot ranges (task 1.3).
    // Fallback to MachineRegistry layout when no item hatches are built.
    int input_start = 0, input_end = 0;
    SimulationEngine::getInputSlotRange(ctrl, input_start, input_end);
    int output_start = 0, output_end = 0;
    SimulationEngine::getOutputSlotRange(ctrl, output_start, output_end);
    if (input_end == 0) {
        if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id)) {
            input_end = std::min(minfo->slots_in, static_cast<int>(container.slots.size()));
        }
    }
    if (output_end == 0) {
        if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id)) {
            output_start = minfo->slots_in;
            output_end = std::min(minfo->slots_in + minfo->slots_out,
                                  static_cast<int>(container.slots.size()));
        }
    }
    const int input_end_capped = std::min(input_end, static_cast<int>(container.slots.size()));
    const int output_end_capped = std::min(output_end, static_cast<int>(container.slots.size()));

    if (!progress.recipe_id.empty()) {
        auto* recipe = recipes_->getRecipeById(progress.recipe_id);
        if (!recipe) {
            progress.recipe_id.clear();
            progress.is_processing = false;
            return;
        }

        const bool orchestrated = reservations_ && recipe->hasResourceRequirements();
        const int32_t perTickCost = orchestrated
            ? static_cast<int32_t>(recipe->resourceAmountPerTick())
            : static_cast<int32_t>(recipe->energy_cost);

        if (energy.current < perTickCost) {
            if (orchestrated) {
                // 4.3.4: recurring charge through the accepted-amount path
                // before progress may advance.
                if (!reservations_->hasOutstandingCharge(entity)) {
                    reservations_->beginPerTickCharge(entity, *recipe);
                }
            } else if (pipeClient_) {
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

        energy.current -= perTickCost;
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
                for (int i = output_start; i < output_end_capped && remaining > 0; ++i) {
                    auto& slot = container.slots[i];
                    if (slot.item_id == out.item_id && slot.meta == out.metadata && slot.count < 64) {
                        uint8_t space = 64 - slot.count;
                        uint8_t add = std::min(remaining, space);
                        slot.count += add;
                        remaining -= add;
                    }
                }
                for (int i = output_start; i < output_end_capped && remaining > 0; ++i) {
                    auto& slot = container.slots[i];
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
        for (int i = input_start; i < input_end_capped; ++i) {
            auto& slot = container.slots[i];
            if (slot.item_id != 0) {
                inputItems.push_back({slot.item_id, slot.count, slot.meta});
            }
        }

        auto* recipe = recipes_->findRecipeByInputs(machine.machine_id, inputItems);
        if (recipe) {
            if (reservations_ && recipe->hasResourceRequirements()) {
                // 4.2.2/4.3.2: reserve before touching inputs or progress.
                tickOrchestratedStart(entity, machine, *recipe,
                                      input_start, input_end_capped);
            } else {
                progress.recipe_id = recipe->id;
                progress.remaining_ticks = static_cast<int32_t>(recipe->duration);
                progress.is_processing = true;

                for (const auto& req : recipe->inputs) {
                    if (req.item_id == 0) continue;
                    int64_t remaining = static_cast<int64_t>(req.count);
                    for (int i = input_start; i < input_end_capped && remaining > 0; ++i) {
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
    }

    float pct = 0.0f;
    if (!progress.recipe_id.empty()) {
        auto* recipe = recipes_->getRecipeById(progress.recipe_id);
        if (recipe && recipe->duration > 0) {
            pct = 1.0f - static_cast<float>(progress.remaining_ticks) / static_cast<float>(recipe->duration);
        }
    }
    auto inv_data = packInventorySlots(container);
    auto hatches = buildHatchUpdateData(ctrl, container);
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
        input_end, // split inventory into input/output grids at the ITEM_IN range
        0.0f,
        &hatches);
}

void LCRSystem::tickOrchestratedStart(entt::entity entity,
                                      MachineComponent& machine,
                                      const RecipeManager::Recipe& recipe,
                                      int input_start, int input_end) {
    auto& progress = reg_.get<RecipeProgress>(entity);

    // 4.2.4: stale pending craft for a different recipe is cancelled.
    if (progress.pending_craft && progress.pending_craft->recipe_id != recipe.id) {
        reservations_->cancel(entity, "recipe changed");
    }

    if (!progress.pending_craft) {
        reservations_->beginReservation(entity, recipe, totalTicks_);
        return;
    }
    if (!reservations_->tickPending(entity, totalTicks_)) {
        return;
    }
    if (progress.pending_craft && progress.pending_craft->fullyAccepted()) {
        auto& container = reg_.get<InventoryContainer>(entity);
        const auto* live = recipes_->getRecipeById(progress.pending_craft->recipe_id);
        if (!live) {
            reservations_->cancel(entity, "recipe vanished");
            return;
        }
        // Commit point (4.3.2): consume inputs exactly once, then start.
        for (const auto& req : live->inputs) {
            if (req.item_id == 0) continue;
            int64_t remaining = static_cast<int64_t>(req.count);
            for (int i = input_start; i < input_end && remaining > 0; ++i) {
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
        progress.recipe_id = live->id;
        progress.remaining_ticks = static_cast<int32_t>(live->duration);
        progress.is_processing = true;
        progress.clearPendingCraft();
    }
}

void LCRSystem::commitPendingCraft(entt::entity entity) {
    auto* machine = reg_.try_get<MachineComponent>(entity);
    auto* progress = reg_.try_get<RecipeProgress>(entity);
    if (!machine || !progress || !progress->pending_craft) return;
    if (!progress->pending_craft->fullyAccepted()) return;

    auto view = reg_.view<const Position, MachineComponent>();
    for (auto e : view) {
        auto& pos = view.get<const Position>(e);
        if (pos.x == machine->x && pos.y == machine->y && pos.z == machine->z) {
            auto it = std::find_if(
                controllers_.begin(), controllers_.end(),
                [&](const auto& entry) {
                    return entry.second.x == machine->x &&
                           entry.second.y == machine->y &&
                           entry.second.z == machine->z;
                });
            if (it == controllers_.end()) return;
            int input_start = 0, input_end = 0;
            SimulationEngine::getInputSlotRange(it->second, input_start, input_end);
            if (input_end == 0) {
                if (auto* minfo = MachineRegistry::instance()->Get(machine->machine_id)) {
                    input_end = std::min(minfo->slots_in,
                                         static_cast<int>(reg_.get<InventoryContainer>(entity).slots.size()));
                }
            }
            const auto* pending_recipe =
                recipes_->getRecipeById(progress->pending_craft->recipe_id);
            if (!pending_recipe) {
                reservations_->cancel(entity, "recipe vanished");
                return;
            }
            tickOrchestratedStart(entity, *machine, *pending_recipe,
                                  input_start,
                                  std::min(input_end, static_cast<int>(reg_.get<InventoryContainer>(entity).slots.size())));
            return;
        }
    }
}

} // namespace simcore
