#include "RecipeCompletedHandler.h"
#include <engine/sim/SimulationEngine.h>
#include "recipe_generated.h"
#include <engine/sim/components/Block.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

namespace {

// An entity that is a candidate receiver of the result: it sits at the event's
// position, and — when the event names one — it is of the named machine type.
//
// RecipeCompleted carries machine_id (recipe.fbs:139) and reciped echoes back
// the id it matched the recipe by (RecipeManagerService.cpp:127,160), so the
// id is real identity information, not a hint. Matching on it is what stops a
// co-located machine of a DIFFERENT type from being wiped by a result meant
// for its neighbour.
//
// machine_id == 0 means "unnamed" (the field is optional on the wire and
// defaults to 0), so the id is not used to filter in that case and the old
// position-only behaviour stands.
struct Candidate {
    entt::entity entity = entt::null;
    // True when the block layer still says this entity owns the block: a live
    // machine entity carries a Block whose id matches its machine_id
    // (SimulationEngine::onBlockChanged emplaces both together), whereas a
    // stale leftover from a rebuild does not. This is the tiebreak when two
    // duplicates of the SAME type sit at one position.
    bool owns_block = false;
};

bool collectCandidates(entt::registry& reg, int32_t x, int32_t y, int32_t z,
                       uint16_t machine_id, std::vector<Candidate>& out) {
    auto view = reg.view<MachineComponent, InventoryContainer>();
    for (auto entity : view) {
        auto& mc = view.get<MachineComponent>(entity);
        if (static_cast<int32_t>(mc.x) != x ||
            static_cast<int32_t>(mc.y) != y ||
            static_cast<int32_t>(mc.z) != z) {
            continue;
        }
        if (machine_id != 0 && mc.machine_id != machine_id) continue;

        bool owns_block = false;
        if (const auto* blk = reg.try_get<Block>(entity)) {
            owns_block = blk->id == mc.machine_id;
        }
        out.push_back({entity, owns_block});
    }
    return !out.empty();
}

} // namespace

RecipeCompletedHandler::RecipeCompletedHandler(std::shared_ptr<SimulationEngine> engine)
    : engine_(std::move(engine))
{}

void RecipeCompletedHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::RecipeCompleted>(nullptr)) {
        spdlog::warn("[SimCore] invalid RecipeCompleted buffer");
        return;
    }
    auto* completed = flatbuffers::GetRoot<Protocol::RecipeCompleted>(data.data());
    if (!completed || !completed->pos() || !completed->result_slots()) return;

    auto* pos = completed->pos();
    int32_t x = pos->x(), y = pos->y(), z = pos->z();
    const uint16_t machine_id = completed->machine_id();

    // Resolve the ONE entity that owns this result before touching anything.
    // The replacement below is destructive (inv.slots.clear()), so picking the
    // wrong entity destroys a machine's contents. Selection is by identity —
    // the machine_id the event names, then the entity that still owns the
    // block — never by the order the entt view happens to yield entities,
    // which is reverse creation order and arbitrary from the caller's view.
    std::vector<Candidate> candidates;
    if (!engine_ || !collectCandidates(engine_->reg(), x, y, z, machine_id, candidates)) {
        // Either there is no engine, or nothing at this position matches. When
        // the event NAMED a machine and no entity of that type is there, the
        // block was replaced mid-craft: drop the event rather than guess, or
        // the result lands on whatever machine happens to share the position.
        if (machine_id != 0) {
            spdlog::warn(
                "[SimCore] RecipeCompleted for machine {} at ({},{},{}) has no "
                "matching entity — dropping result",
                machine_id, x, y, z);
        }
        return;
    }

    entt::entity target = entt::null;
    for (const auto& c : candidates) {
        if (c.owns_block) { target = c.entity; break; }
    }
    if (target == entt::null) {
        // No duplicate claimed the block (a single machine, or several stale
        // leftovers): the first match is the only candidate that matters.
        target = candidates.front().entity;
    }
    if (candidates.size() > 1) {
        spdlog::warn(
            "[SimCore] {} co-located entities match machine {} at ({},{},{}); "
            "applied the result to entity {}",
            candidates.size(), machine_id, x, y, z, static_cast<uint32_t>(target));
    }

    auto& inv = engine_->reg().get<InventoryContainer>(target);
    inv.slots.clear();
    auto* slots = completed->result_slots();
    for (uint16_t i = 0; i < slots->size(); ++i) {
        auto* s = slots->Get(i);
        inv.slots.push_back({s->item_id(),
                              static_cast<uint8_t>(s->count()),
                              s->meta()});
    }
    spdlog::info("[SimCore] Applied recipe result at ({},{},{})", x, y, z);
}

} // namespace simcore
