#include "MachineSystem.h"
#include "../../Storage/InventorySerializer.h"
#include "Network/PipeEnergyClient.h"
#include "Saturate.h"
#include "MachineRegistry.h"
#include "../components/OverheatComponent.h"
#include "../components/HeatSlowComponent.h"

namespace simcore {

namespace {
    RecipeManager::ItemStack invSlotToItem(const InventorySlot& slot) {
        return {slot.item_id, slot.count, slot.meta};
    }

    std::vector<RecipeManager::ItemStack> invSlotsToItems(const std::vector<InventorySlot>& slots) {
        std::vector<RecipeManager::ItemStack> items;
        items.reserve(slots.size());
        for (const auto& s : slots) items.push_back(invSlotToItem(s));
        return items;
    }

    InventorySlot itemToInvSlot(const RecipeManager::ItemStack& item) {
        return {item.item_id, item.count, item.metadata};
    }

    std::vector<InventorySlot> itemsToInvSlots(const std::vector<RecipeManager::ItemStack>& items) {
        std::vector<InventorySlot> slots;
        slots.reserve(items.size());
        for (const auto& it : items) slots.push_back(itemToInvSlot(it));
        return slots;
    }
}

MachineSystem::MachineSystem(entt::registry& reg,
                              std::shared_ptr<RecipeManager::RecipeManager> recipes,
                              std::shared_ptr<IEventPublisher> events,
                              std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), recipes_(recipes), events_(events), pipeClient_(pipeClient)
{
}

void MachineSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, RecipeProgress, InventoryContainer, EnergyStorage>();

    // Periodic force-publish so reconnecting clients catch up on machine state
    ++tickCounter_;
    bool forcePublish = (tickCounter_ >= kForcePublishInterval);
    if (forcePublish) tickCounter_ = 0;

    // ---- Pass 0: publish inventory state for ALL machines ----
    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& progress = view.get<RecipeProgress>(ent);
        auto& container = view.get<InventoryContainer>(ent);
        auto& energy = view.get<EnergyStorage>(ent);
        (void)progress;

        // Compute inventory hash to detect changes
        uint64_t hash = 0;
        for (const auto& slot : container.slots) {
            hash ^= static_cast<uint64_t>(slot.item_id) << 0
                  ^ static_cast<uint64_t>(slot.count)   << 16
                  ^ static_cast<uint64_t>(slot.meta)    << 24;
            hash ^= hash >> 12;
        }
        auto prev = lastInventoryHash_[static_cast<uint64_t>(ent)];
        if (!forcePublish && hash == prev) continue; // no change → skip
        lastInventoryHash_[static_cast<uint64_t>(ent)] = hash;

        std::vector<uint8_t> inv_data = packInventory(container.slots);

        int slt_in = 0;
        if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id))
            slt_in = minfo->slots_in;
        events_->publishBlockEntityUpdate(
            static_cast<int32_t>(machine.x),
            static_cast<int32_t>(machine.y),
            static_cast<int32_t>(machine.z),
            machine.machine_id,
            inv_data,
            1.0f,  // progress 1.0 = waiting for recipe
            energy.current,
            static_cast<EnergyType>(energy.type),
            static_cast<uint32_t>(energy.capacity),
            slt_in);
    }

    // ---- Pass 1: find new recipes for idle machines (skip managed_externally) ----
    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& progress = view.get<RecipeProgress>(ent);
        auto& container = view.get<InventoryContainer>(ent);
        auto& energy = view.get<EnergyStorage>(ent);
        (void)energy;

        if (machine.managed_externally) continue;
        if (!progress.recipe_id.empty()) {
            continue;
        }

        // Add overheat check
        auto* oh = reg_.try_get<OverheatComponent>(ent);
        if (oh && oh->state != OverheatState::NONE) continue;

        auto inputItems = invSlotsToItems(container.slots);
        auto* recipe = recipes_->findRecipeByInputs(machine.machine_id, inputItems);

        if (recipe) {
            if (RecipeManager::evaluateConditions(recipe->id, reg_,
                                             machine.x, machine.y, machine.z, *recipes_)) {
                progress.recipe_id = recipe->id;
                progress.remaining_ticks = recipe->duration;
                progress.is_processing = true;
            }
            continue;
        }

        if (!progress.is_processing && !progress.needs_output) {
            continue;
        }
    }

    // ---- Pass 2: tick active recipes (skip managed_externally) ----
    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& progress = view.get<RecipeProgress>(ent);
        auto& container = view.get<InventoryContainer>(ent);
        auto& energy = view.get<EnergyStorage>(ent);

        if (machine.managed_externally) continue;
        if (progress.recipe_id.empty()) {
            continue;
        }

        // Add overheat speed control
        auto* oh = reg_.try_get<OverheatComponent>(ent);
        if (oh) {
            if (oh->state == OverheatState::CRITICAL) continue; // STOP
            if (oh->state == OverheatState::WARNING) {           // 50% speed
                auto* slow = reg_.try_get<HeatSlowComponent>(ent);
                if (!slow) slow = &reg_.emplace<HeatSlowComponent>(ent);
                slow->accumulator += 1.0f;
                if (slow->accumulator < 2.0f) continue;
                slow->accumulator = 0.0f;
            }
        } else {
            if (reg_.all_of<HeatSlowComponent>(ent))
                reg_.remove<HeatSlowComponent>(ent);
        }

        auto* recipe = recipes_->getRecipeById(progress.recipe_id);
        if (!recipe) {
            progress.is_processing = false;
            progress.needs_output = false;
            progress.recipe_id.clear();
            continue;
        }

        if (energy.current < static_cast<int32_t>(recipe->energy_cost)) {
            // HEAT machines use PipeNetwork continuous energy flow
            // ELECTRICITY machines use CableGraph packet-based transport
            if (energy.type == EnergyType::HEAT) {
                uint64_t node_id = static_cast<uint64_t>(ent);
                auto pending_it = pendingConsumes_.find(node_id);
                if (pending_it == pendingConsumes_.end()) {
                    int32_t needed = static_cast<int32_t>(recipe->energy_cost);
                    if (pipeClient_) {
                        pipeClient_->sendConsumeRequest(
                            node_id,
                            static_cast<int32_t>(machine.x),
                            static_cast<int32_t>(machine.y),
                            static_cast<int32_t>(machine.z),
                            static_cast<int32_t>(energy.type),
                            needed
                        );
                    }
                    pendingConsumes_[node_id] = needed;
                    spdlog::debug("Heat machine {} at entity {} requested {} energy from PipeNetwork",
                                 recipe->id, static_cast<uint32_t>(ent), needed);
                }
            } else if (energy.type == EnergyType::ELECTRICITY) {
                uint64_t node_id = static_cast<uint64_t>(ent);
                auto pending_it = pendingConsumes_.find(node_id);
                if (pending_it == pendingConsumes_.end()) {
                    int32_t needed = static_cast<int32_t>(recipe->energy_cost);
                    if (pipeClient_) {
                        pipeClient_->sendConsumeRequest(
                            node_id,
                            static_cast<int32_t>(machine.x),
                            static_cast<int32_t>(machine.y),
                            static_cast<int32_t>(machine.z),
                            static_cast<int32_t>(energy.type),
                            needed
                        );
                    }
                    pendingConsumes_[node_id] = needed;
                    spdlog::debug("Electricity machine {} at entity {} requested {} energy from CableGraph",
                                 recipe->id, static_cast<uint32_t>(ent), needed);
                }
            }
            continue;
        }
        energy.current = simcore::sub_sat(energy.current, static_cast<int32_t>(recipe->energy_cost));
        progress.remaining_ticks--;

        // Notify PipeNetwork of energy state change
        if (pipeClient_) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
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
                true
            );
        }

        if (progress.remaining_ticks == 0) {
            auto inputItems = invSlotsToItems(container.slots);
            auto newItems = recipe->craft(inputItems);
            container.slots = itemsToInvSlots(newItems);

            progress.is_processing = false;
            progress.needs_output = true;
            progress.recipe_id.clear();

            spdlog::debug("Machine {} at {} completed recipe {} and produced items",
                         recipe->id, static_cast<unsigned int>(ent), recipe->id);
        }

        // Publish progress so the client can render the progress bar & energy.
        {
            float pct = (recipe->duration == 0) ? 0.0f
                : 1.0f - static_cast<float>(progress.remaining_ticks) / static_cast<float>(recipe->duration);
            std::vector<uint8_t> inv_data = packInventory(container.slots);
            int slt_in = 0;
            if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id))
                slt_in = minfo->slots_in;
            events_->publishBlockEntityUpdate(
                machine.x, machine.y, machine.z,
                machine.machine_id,
                inv_data,
                pct,
                static_cast<uint32_t>(energy.current),
                energy.type,
                static_cast<uint32_t>(energy.capacity),
                slt_in);
        }
    }
}

void MachineSystem::onConsumeResponse(uint64_t node_id, int32_t consumed, int32_t /*remaining*/) {
     if (consumed <= 0) {
         if (node_id != 0) {
             pendingConsumes_.erase(node_id);
         }
         return;
     }

     // If node_id provided, use it for direct lookup (FIFO correlation)
     if (node_id != 0) {
         entt::entity ent = static_cast<entt::entity>(node_id);
         if (!reg_.valid(ent)) {
             pendingConsumes_.erase(node_id);
             return;
         }

         auto* machine = reg_.try_get<MachineComponent>(ent);
         auto* progress = reg_.try_get<RecipeProgress>(ent);
         auto* energy = reg_.try_get<EnergyStorage>(ent);
         if (!machine || !progress || !energy) {
             pendingConsumes_.erase(node_id);
             return;
         }

         energy->current = (std::min)(simcore::add_sat(energy->current, consumed), energy->capacity);

         spdlog::debug("Machine {} at entity {} received {} energy from PipeNetwork (total: {})",
                       progress->recipe_id, static_cast<uint32_t>(ent), consumed, energy->current);
         pendingConsumes_.erase(node_id);
         return;
     }

     // No node_id: process in FIFO order
     if (pendingConsumes_.empty()) {
         return;
     }

     // Get oldest pending entity
     entt::entity oldest_ent = static_cast<entt::entity>(pendingConsumes_.begin()->first);
     auto* machine = reg_.try_get<MachineComponent>(oldest_ent);
     auto* progress = reg_.try_get<RecipeProgress>(oldest_ent);
     auto* energy = reg_.try_get<EnergyStorage>(oldest_ent);
     if (!machine || !progress || !energy) {
         pendingConsumes_.erase(pendingConsumes_.begin());
         return;
     }

     energy->current = (std::min)(simcore::add_sat(energy->current, consumed), energy->capacity);

      spdlog::debug("Machine {} at entity {} received {} energy from PipeNetwork (total: {})",
                    progress->recipe_id, static_cast<uint32_t>(oldest_ent), consumed, energy->current);
     pendingConsumes_.erase(pendingConsumes_.begin());
 }

} // namespace simcore
