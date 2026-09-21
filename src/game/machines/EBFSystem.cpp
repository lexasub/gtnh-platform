#include "EBFSystem.h"
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/RecipeProgress.h>
#include <engine/sim/components/HeatIntakeComponent.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/MultiblockController.h>
#include "Network/IEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "Network/CraftReservationClient.h"
#include <engine/sim/SimulationEngine.h>
#include "ECS/MultiblockUtils.h"
#include "RecipeManager/RecipeManager.h"
#include <game/recipes/RecipeTypes.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>

namespace simcore {

EBFSystem::EBFSystem(entt::registry& reg,
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

void EBFSystem::tick(float) {
    ++totalTicks_;
    std::vector<uint64_t> ids;
    for (const auto& [id, _] : controllers_) ids.push_back(id);

    for (uint64_t ctrl_id : ids) {
        auto it = controllers_.find(ctrl_id);
        if (it == controllers_.end()) continue;
        if (it->second.id == 0) continue;
        if (it->second.pattern_id != 1 && it->second.pattern_id != 4) continue;
        tickEBF(ctrl_id, it->second);
    }
}

int EBFSystem::getCoilHeat(uint16_t block_id) const {
    if (block_id == KANHAL_COIL_BLOCK_ID ||
        block_id == LEGACY_KANHAL_COIL_BLOCK_ID) {
        return KANHAL_MAX_HEAT;
    }
    if (block_id == NICHROME_COIL_BLOCK_ID || block_id == 1007) {
        return NICHROME_MAX_HEAT;
    }
    if (block_id == TUNGSTENSTEEL_COIL_BLOCK_ID || block_id == 1008) {
        return TUNGSTENSTEEL_MAX_HEAT;
    }
    return 0;
}

int EBFSystem::detectHeatTier(const MultiblockController& ctrl) const {
    const auto* pattern = patterns_.getPattern(ctrl.pattern_id);
    if (!pattern) return KANHAL_MAX_HEAT;

    // Coil blocks sit at corner-relative (1, COIL_LAYER_1, 1) and
    // (1, COIL_LAYER_2, 1); controller at (controller_dx, controller_dy,
    // controller_dz). coil world = corner + (1, dy, 1), corner = controller − offset.
    int32_t corner_x = static_cast<int32_t>(ctrl.x) - pattern->controller_dx;
    int32_t corner_y = static_cast<int32_t>(ctrl.y) - pattern->controller_dy;
    int32_t corner_z = static_cast<int32_t>(ctrl.z) - pattern->controller_dz;

    int minHeat = std::numeric_limits<int>::max();
    for (int dy = COIL_LAYER_1; dy <= COIL_LAYER_2; ++dy) {
        uint32_t wx = static_cast<uint32_t>(corner_x + COIL_DX);
        uint32_t wy = static_cast<uint32_t>(corner_y + dy);
        uint32_t wz = static_cast<uint32_t>(corner_z + COIL_DZ);

        uint16_t block_id = 0;
        auto view = reg_.view<const Position, const Block>();
        for (auto e : view) {
            auto [pos, blk] = view.get(e);
            if (pos.x == wx && pos.y == wy && pos.z == wz) {
                block_id = blk.id;
                break;
            }
        }
        const int layerHeat = getCoilHeat(block_id);
        if (layerHeat <= 0) return 0;
        minHeat = std::min(minHeat, layerHeat);
    }
    return minHeat == std::numeric_limits<int>::max() ? 0 : minHeat;
}

bool EBFSystem::onConsumeResponse(uint64_t node_id, int32_t consumed, int32_t) {
    auto it = pendingConsumes_.find(node_id);
    if (it == pendingConsumes_.end()) return false;
    pendingConsumes_.erase(it);
    if (consumed <= 0) return true;

    auto entity = static_cast<entt::entity>(node_id);
    auto* energy = reg_.try_get<EnergyStorage>(entity);
    if (!energy) return true;
    energy->current = std::min(energy->capacity, energy->current + consumed);
    return true;
}

void EBFSystem::tickEBF(uint64_t ctrl_id, MultiblockController& ctrl) {
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
    auto& heat = reg_.get_or_emplace<HeatIntakeComponent>(entity);

    // Item IO flows through ITEM_IN/ITEM_OUT hatch slot ranges.
    int input_start = 0, input_end = 0;
    SimulationEngine::getInputSlotRange(ctrl, input_start, input_end);
    int output_start = 0, output_end = 0;
    SimulationEngine::getOutputSlotRange(ctrl, output_start, output_end);

    // Fallback when no item hatches are built: MachineRegistry slot layout.
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

    const bool electric = ctrl.pattern_id == 1;
    const HatchSlot* energy_hatch = nullptr;
    if (electric) {
        for (const auto& hatch : ctrl.hatches) {
            if (hatch.type == HatchType::ENERGY && hatch.present) {
                energy_hatch = &hatch;
                break;
            }
        }
        // An EBF requires a physical ENERGY hatch; a structural role alone
        // must not make an EU endpoint available.
        if (!energy_hatch) return;
    }
    const int32_t endpoint_x = energy_hatch
        ? static_cast<int32_t>(energy_hatch->world_x)
        : static_cast<int32_t>(machine.x);
    const int32_t endpoint_y = energy_hatch
        ? static_cast<int32_t>(energy_hatch->world_y)
        : static_cast<int32_t>(machine.y);
    const int32_t endpoint_z = energy_hatch
        ? static_cast<int32_t>(energy_hatch->world_z)
        : static_cast<int32_t>(machine.z);
    const int coilMaxHeat = electric ? 0 : detectHeatTier(ctrl);
    // HBF is HU-gated by its coils; EBF has no heat requirement.
    const int requiredHeat = electric ? 0 : coilMaxHeat / 2;

    if (pipeClient_) {
        pipeClient_->publishNodeUpdate(
            static_cast<uint64_t>(entity), endpoint_x, endpoint_y, endpoint_z,
            energy.current, energy.capacity, energy.maxInput, energy.maxOutput,
            energy.tier, static_cast<int32_t>(energy.type), false, true);
    }

    if (!progress.recipe_id.empty()) {
        auto* recipe = recipes_->getRecipeById(progress.recipe_id);
        if (!recipe) {
            progress.recipe_id.clear();
            progress.is_processing = false;
            return;
        }

        if (heat.heat_stored < requiredHeat) return; // not hot enough
        if (electric && energy.type != EnergyType::ELECTRICITY) return;
        if (!electric && energy.type != EnergyType::HEAT) return;

        const bool orchestrated = reservations_ && recipe->hasResourceRequirements() &&
                                  !electric;
        const int32_t perTickCost = recipe->hasResourceRequirements()
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
                const uint64_t node_id = static_cast<uint64_t>(entity);
                if (pendingConsumes_.find(node_id) == pendingConsumes_.end()) {
                    pipeClient_->sendConsumeRequest(
                        node_id, endpoint_x, endpoint_y, endpoint_z,
                        static_cast<int32_t>(energy.type),
                        perTickCost);
                    pendingConsumes_[node_id] = perTickCost;
                }
            }
            return;
        }

        energy.current -= perTickCost;
        if (!electric) {
            heat.heat_stored = energy.current;
        }
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
                        slot.count = static_cast<uint8_t>(slot.count + add);
                        remaining = static_cast<uint8_t>(remaining - add);
                    }
                }
                for (int i = output_start; i < output_end_capped && remaining > 0; ++i) {
                    auto& slot = container.slots[i];
                    if (slot.item_id == 0) {
                        slot = {out.item_id, remaining, out.metadata};
                        remaining = 0;
                    }
                }
                if (remaining > 0) {
                    spdlog::warn("[EBF] ITEM_OUT hatch full, {} of item {} dropped",
                                 remaining, out.item_id);
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
        if (recipe && heat.heat_stored >= requiredHeat) {
            if (reservations_ && recipe->hasResourceRequirements()) {
                // 4.2.2/4.3.2: reserve before touching inputs or progress.
                tickOrchestratedStart(entity, *recipe,
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
                            slot.count = static_cast<uint8_t>(slot.count - take);
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
        heat.ratio(),
        &hatches);
}

void EBFSystem::tickOrchestratedStart(entt::entity entity,
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
                    slot.count = static_cast<uint8_t>(slot.count - take);
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

void EBFSystem::commitPendingCraft(entt::entity entity) {
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
            tickOrchestratedStart(entity, *pending_recipe,
                                  input_start,
                                  std::min(input_end, static_cast<int>(reg_.get<InventoryContainer>(entity).slots.size())));
            return;
        }
    }
}

} // namespace simcore
