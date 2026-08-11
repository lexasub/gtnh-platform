#include "MachineSystem.h"
#include "../../Storage/InventorySerializer.h"
#include "Network/PipeEnergyClient.h"
#include "Network/FluidClient.h"
#include "Network/ItemClient.h"
#include <common/ItemId.h>
#include "Saturate.h"
#include "MachineRegistry.h"
#include "../components/OverheatComponent.h"
#include "../components/HeatSlowComponent.h"
#include "../components/HeatIntakeComponent.h"
#include "../components/SteamOutputComponent.h"
#include "Storage/ContainerSession.h"
#include "Storage/ChestStateManager.h"
#include "Storage/PlayerInventoryStore.h"
#include "Network/clients/IoUringRouterClient.h"

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

}

MachineSystem::MachineSystem(entt::registry& reg,
                              std::shared_ptr<RecipeManager::RecipeManager> recipes,
                              std::shared_ptr<IEventPublisher> events,
                              std::shared_ptr<PipeEnergyClient> pipeClient,
                              std::shared_ptr<ItemClient> itemClient,
                              std::shared_ptr<ContainerSessionRegistry> sessions,
                              std::shared_ptr<PlayerInventoryStore> invStore,
                              std::shared_ptr<IoUringRouterClient> router,
                              std::shared_ptr<FluidClient> fluidClient)
    : reg_(reg), recipes_(recipes), events_(events), pipeClient_(pipeClient),
      itemClient_(std::move(itemClient)), sessions_(std::move(sessions)),
      invStore_(std::move(invStore)), router_(std::move(router)),
      fluidClient_(std::move(fluidClient))
{
}

void MachineSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, RecipeProgress, InventoryContainer, EnergyStorage>();

    // Periodic force-publish so reconnecting clients catch up on machine state
    ++tickCounter_;
    bool forcePublish = (tickCounter_ >= kForcePublishInterval);
    if (forcePublish) tickCounter_ = 0;
    // Force-publish on first few ticks to catch late-connecting clients
    if (startupTicks_ > 0) { forcePublish = true; --startupTicks_; }

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
        float heatRatio = 0.0f;
        if (auto* hic = reg_.try_get<HeatIntakeComponent>(ent)) {
            heatRatio = hic->ratio();
        }
        double steamCur = -1.0, steamCap = -1.0;
        if (auto* soc = reg_.try_get<SteamOutputComponent>(ent)) {
            steamCur = soc->steam_stored;
            steamCap = soc->steam_capacity;
        }
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
            slt_in,
            heatRatio,
            nullptr, steamCur, steamCap);
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

        // Resolve input/output slot boundaries from MachineRegistry
        int slots_in  = static_cast<int>(container.slots.size());
        int slots_out = 0;
        if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id)) {
            slots_in  = minfo->slots_in;
            slots_out = minfo->slots_out;
        }
        int total_slots = slots_in + slots_out;

        if (progress.needs_output) {
            bool outputsCleared = true;
            int check_end = std::min(total_slots, static_cast<int>(container.slots.size()));
            for (int i = slots_in; i < check_end; ++i) {
                if (container.slots[i].item_id != 0) { outputsCleared = false; break; }
            }
            if (!outputsCleared) continue;
            progress.needs_output = false;
        }

        // Only use input slots for recipe matching (skip output region)
        std::vector<InventorySlot> inputOnly;
        int input_end = std::min(slots_in, static_cast<int>(container.slots.size()));
        inputOnly.reserve(input_end);
        for (int i = 0; i < input_end; ++i) inputOnly.push_back(container.slots[i]);

        auto inputItems = invSlotsToItems(inputOnly);
        auto* recipe = recipes_->findRecipeByInputs(machine.machine_id, inputItems);

        if (recipe) {
            if (RecipeManager::evaluateConditions(recipe->id, reg_,
                                             machine.x, machine.y, machine.z, *recipes_)) {
                // ── Consume input items immediately when recipe starts ──
                for (const auto& req : recipe->inputs) {
                    if (req.item_id == 0) continue;
                    int64_t remaining = static_cast<int64_t>(req.count);
                    for (int i = 0; i < input_end && remaining > 0; ++i) {
                        auto& slot = container.slots[i];
                        if (slot.item_id == req.item_id && slot.meta == req.metadata) {
                            uint8_t take = std::min(slot.count,
                                static_cast<uint8_t>(remaining));
                            slot.count -= take;
                            remaining -= take;
                            if (slot.count == 0) {
                                slot.item_id = 0;
                                slot.meta = 0;
                            }
                        }
                    }
                }

                // Notify open windows that inventory changed after consume
                publishInventoryIfOpen(machine);

                progress.recipe_id = recipe->id;
                progress.remaining_ticks = recipe->duration;
                progress.is_processing = true;
            }
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
            } else if (energy.type == EnergyType::ROTATION) {
                uint64_t node_id = static_cast<uint64_t>(ent);
                auto pending_it = pendingConsumes_.find(node_id);
                if (pending_it == pendingConsumes_.end()) {
                    int32_t needed = static_cast<int32_t>(recipe->energy_cost);
                    if (pipeClient_) {
                        // Publish the node update first: PipeNetwork registers
                        // sink nodes only on demand, and the consume request
                        // is rejected until the node is known.
                        pipeClient_->publishNodeUpdate(
                            node_id,
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
                    spdlog::debug("Rotation machine {} at entity {} requested {} energy from PipeNetwork",
                                 recipe->id, static_cast<uint32_t>(ent), needed);
                }
            } else if (energy.type == EnergyType::STEAM) {
                uint64_t node_id = static_cast<uint64_t>(ent);
                auto pit = pendingFluidConsumes_.find(node_id);
                if (pit == pendingFluidConsumes_.end()) {
                    int32_t needed = static_cast<int32_t>(recipe->energy_cost);
                    if (fluidClient_) {
                        // register node with fluid capacity first (pipes attach via fluid.node.update)
                        fluidClient_->publishNodeUpdate(
                            node_id,
                            static_cast<int32_t>(machine.x),
                            static_cast<int32_t>(machine.y),
                            static_cast<int32_t>(machine.z),
                            ItemId::pack("1111:11:1"),
                            energy.current,
                            energy.capacity,
                            0, 0, energy.tier, false, true);   // sink/neutral
                        fluidClient_->sendFluidRequest(
                            node_id,
                            static_cast<int32_t>(machine.x),
                            static_cast<int32_t>(machine.y),
                            static_cast<int32_t>(machine.z),
                            ItemId::pack("1111:11:1"),
                            needed);
                    }
                    pendingFluidConsumes_[node_id] = needed;
                    spdlog::debug("Steam machine {} at entity {} requested {} steam from PipeNetwork",
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
            // Place recipe outputs into output slot region
            auto* minfo = MachineRegistry::instance()->Get(machine.machine_id);
            int slots_in  = minfo ? minfo->slots_in : 0;
            int slots_out = minfo ? minfo->slots_out
                : static_cast<int>(container.slots.size()) - slots_in;
            int total_slots = slots_in + slots_out;

            for (const auto& out : recipe->outputs) {
                uint8_t remaining = out.count;
                if (remaining == 0) continue;

                // 1) Try stacking onto existing matching output
                int stack_end = std::min(total_slots, static_cast<int>(container.slots.size()));
                for (int i = slots_in; i < stack_end && remaining > 0; ++i) {
                    auto& slot = container.slots[i];
                    if (slot.item_id == out.item_id && slot.meta == out.metadata && slot.count < 64) {
                        uint8_t space = 64 - slot.count;
                        uint8_t add = std::min(remaining, space);
                        slot.count += add;
                        remaining -= add;
                    }
                }
                // 2) Fill empty output slots
                for (int i = slots_in; i < stack_end && remaining > 0; ++i) {
                    auto& slot = container.slots[i];
                    if (slot.item_id == 0) {
                        slot = {out.item_id, remaining, out.metadata};
                        remaining = 0;
                    }
                }
                if (remaining > 0) {
                    spdlog::warn("Machine {} at entity {}: output slots full, {} of item {} dropped",
                                 recipe->id, static_cast<uint32_t>(ent), remaining, out.item_id);
                }
            }

            progress.is_processing = false;
            progress.needs_output = true;
            progress.recipe_id.clear();

            {
                // Diagnostic: check output slots after placement
                int diag_out = 0;
                for (int di = slots_in; di < static_cast<int>(container.slots.size()); ++di) {
                    if (container.slots[di].item_id != 0) ++diag_out;
                }
                spdlog::debug("[Diagnostic] Recipe {} completed at ({},{},{}): {} output slots have items ({} slots_in, {} total)",
                              recipe->id, machine.x, machine.y, machine.z,
                              diag_out, slots_in, static_cast<int>(container.slots.size()));
            }

            spdlog::debug("Machine {} at {} completed recipe {} and produced items",
                          recipe->id, static_cast<unsigned int>(ent), recipe->id);

            // Push output items into adjacent pipe network
            pushOutputToPipe(static_cast<uint64_t>(ent), machine, container, slots_in);

            // Notify open windows that inventory changed after recipe completion
            publishInventoryIfOpen(machine);
        }

        // Publish progress so the client can render the progress bar & energy.
        {
            float pct = (recipe->duration == 0) ? 0.0f
                : 1.0f - static_cast<float>(progress.remaining_ticks) / static_cast<float>(recipe->duration);
            std::vector<uint8_t> inv_data = packInventory(container.slots);
            int slt_in = 0;
            if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id))
                slt_in = minfo->slots_in;
            float heatRatio = 0.0f;
            if (auto* hic = reg_.try_get<HeatIntakeComponent>(ent)) {
                heatRatio = hic->ratio();
            }
            events_->publishBlockEntityUpdate(
                machine.x, machine.y, machine.z,
                machine.machine_id,
                inv_data,
                pct,
                static_cast<uint32_t>(energy.current),
                energy.type,
                static_cast<uint32_t>(energy.capacity),
                slt_in,
                heatRatio);
        }
    }
}

void MachineSystem::pushOutputToPipe(uint64_t entity_id, const MachineComponent& machine,
                                      InventoryContainer& container, int slots_in) {
    if (!itemClient_) return;

    std::vector<uint16_t> item_ids;
    std::vector<uint8_t> item_counts;
    for (size_t i = static_cast<size_t>(slots_in); i < container.slots.size(); ++i) {
        if (container.slots[i].item_id != 0 && container.slots[i].count > 0) {
            item_ids.push_back(container.slots[i].item_id);
            item_counts.push_back(container.slots[i].count);
        }
    }

    if (item_ids.empty()) return;

    itemClient_->publishNodeUpdate(
        entity_id,
        static_cast<int32_t>(machine.x),
        static_cast<int32_t>(machine.y),
        static_cast<int32_t>(machine.z),
        item_ids, item_counts,
        0,    // machine node, not a pipe buffer
        true, // items flowing out
        false,
        {}    // PipeNetworkService auto-detects adjacency
    );

    spdlog::debug("MachineSystem: pushed {} item types to pipe network from machine at ({},{},{})",
                  item_ids.size(), machine.x, machine.y, machine.z);
}

void MachineSystem::publishInventoryIfOpen(const MachineComponent& mc) {
    if (!sessions_ || !router_ || !invStore_) return;
    int32_t x = static_cast<int32_t>(mc.x);
    int32_t y = static_cast<int32_t>(mc.y);
    int32_t z = static_cast<int32_t>(mc.z);
    sessions_->forEachOpenAt(x, y, z, [&](uint64_t pid, ContainerSession& s) {
        PublishFullInventory(router_, *invStore_, s, pid, x, y, z);
    });
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

         // Pipe-fed HEAT machines (e.g. steam_heat_boiler pulling via BoilerSystem)
         // keep HeatIntakeComponent synced to EnergyStorage.current, exactly like
         // AdjacencyTransferSystem does for neighbour-transferred heat.
         if (energy->type == EnergyType::HEAT) {
             if (auto* hic = reg_.try_get<HeatIntakeComponent>(ent)) {
                 hic->heat_stored = energy->current;
             }
         }
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

    if (energy->type == EnergyType::HEAT) {
        if (auto* hic = reg_.try_get<HeatIntakeComponent>(oldest_ent)) {
            hic->heat_stored = energy->current;
        }
    }

    spdlog::debug("Machine {} at entity {} received {} energy from PipeNetwork (total: {})",
                  progress->recipe_id, static_cast<uint32_t>(oldest_ent), consumed, energy->current);
    pendingConsumes_.erase(pendingConsumes_.begin());
}
void MachineSystem::onFluidConsumeResponse(int32_t consumed) {
     if (consumed <= 0) {
         if (!pendingFluidConsumes_.empty()) {
             pendingFluidConsumes_.erase(pendingFluidConsumes_.begin());
             if (pendingFluidConsumes_.empty()) pendingFluidConsumes_.clear();
         }
         return;
     }

     // FluidConsumeResp carries no node id: process in FIFO order
     if (pendingFluidConsumes_.empty()) {
         return;
     }

     // Get oldest pending entity
     entt::entity oldest_ent = static_cast<entt::entity>(pendingFluidConsumes_.begin()->first);
     auto* machine = reg_.try_get<MachineComponent>(oldest_ent);
     auto* progress = reg_.try_get<RecipeProgress>(oldest_ent);
     auto* energy = reg_.try_get<EnergyStorage>(oldest_ent);
     if (!machine || !progress || !energy) {
         pendingFluidConsumes_.erase(pendingFluidConsumes_.begin());
         return;
     }

     energy->current = (std::min)(simcore::add_sat(energy->current, consumed), energy->capacity);

     spdlog::debug("Machine {} at entity {} received {} steam from PipeNetwork (total: {})",
                   progress->recipe_id, static_cast<uint32_t>(oldest_ent), consumed, energy->current);
     pendingFluidConsumes_.erase(pendingFluidConsumes_.begin());
 }

} // namespace simcore
