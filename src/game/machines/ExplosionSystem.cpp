#include "ExplosionSystem.h"
#include <game/machines/HeatSlowComponent.h>
#include <engine/sim/components/MachineComponent.h>
#include <game/machines/OverheatComponent.h>
#include <engine/sim/components/Position.h>
#include "HeatConstants.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace simcore {

namespace {

// gp-dd5q: squared Euclidean distance between two block positions.
//
// Subtraction is done in signed 64-bit: `Position` coordinates are uint32_t, so
// a raw `a - b` would wrap around for positions on opposite sides of the world
// origin and turn a far-away block into a near one. The result is compared
// against a squared reach, so the blast needs no sqrt and is exactly spherical
// (a per-axis/cubical test would wrongly destroy a diagonal at d = 2.83).
int64_t squaredDistance(int32_t ax, int32_t ay, int32_t az,
                        int32_t bx, int32_t by, int32_t bz) {
  const int64_t dx = static_cast<int64_t>(ax) - static_cast<int64_t>(bx);
  const int64_t dy = static_cast<int64_t>(ay) - static_cast<int64_t>(by);
  const int64_t dz = static_cast<int64_t>(az) - static_cast<int64_t>(bz);
  return dx * dx + dy * dy + dz * dz;
}

// The falloff, as a reach in blocks for a given target kind. Multiblock
// anchors are reinforced and take the shorter reach; single-block machines are
// the softest thing the blast destroys and take the full radius.
int32_t reachFor(bool isMultiblockAnchor) {
  return isMultiblockAnchor ? HeatConstants::EXPLOSION_ANCHOR_REACH
                            : HeatConstants::EXPLOSION_SINGLEBLOCK_REACH;
}

} // namespace

void ExplosionSystem::tick(float /*dt*/) {
  // Candidate gate: MachineComponent + Position + OverheatComponent, and the
  // entity must be a live multiblock controller (MachineComponent::mb_id != 0).
  //
  // This used to be a 4-type view keyed on a `MultiblockController` ECS
  // component (gp-qgtc / gp-wjwr). That component is never emplaced anywhere
  // in production: SimulationEngine owns controllers in a plain
  // `std::unordered_map<uint64_t, MultiblockController> controllers_`
  // (SimulationEngine.h:108) which EBFSystem / LCRSystem / LargeBoilerSystem
  // mutate in place, so no ECS mirror could stay in sync with it. Keying off
  // `mb_id` instead uses the field the engine already maintains:
  //   * set to the controller id on formation  (SimulationEngine.cpp:309)
  //   * updated on every block-change echo     (SimulationEngine.cpp:370)
  //   * removed with the MachineComponent when the multiblock is destroyed
  //     (destroyController, SimulationEngine.cpp:82)
  // Only the anchor block is ever a machine (member blocks are casing/coil,
  // not machines), so `mb_id != 0` is exactly "is a controller anchor".
  auto view = reg_.view<MachineComponent, Position, OverheatComponent>();

  // Epicentres are resolved first, then the blast wave is swept. Splitting the
  // two is what makes the blast non-chaining: a machine caught in a blast is
  // destroyed, never re-entered as a fuse or as an epicentre. It also makes the
  // sweep well defined when two epicentres overlap — a victim is destroyed at
  // most once.
  std::vector<entt::entity> epicentres;

  for (auto ent : view) {
    auto& machine = view.get<MachineComponent>(ent);
    auto& overheat = view.get<OverheatComponent>(ent);
    auto& pos = view.get<Position>(ent);

    // Only multiblock controller anchors explode — single-block machines
    // are out of the view's original intent (see the note above). They are
    // still valid BLAST TARGETS, just never epicentres.
    if (machine.mb_id == 0) continue;
    if (overheat.state != OverheatState::CRITICAL) continue;

    overheat.ticks_at_critical++;
    if (overheat.ticks_at_critical < HeatConstants::EXPLOSION_DELAY_TICKS) continue;

    spdlog::warn("[Explosion] Machine at ({},{},{}) exploded!", pos.x, pos.y, pos.z);

    // The epicentre's own block is always cleared, whatever the falloff says.
    events_->publishBlockChangedEvent(static_cast<int32_t>(pos.x),
                                      static_cast<int32_t>(pos.y),
                                      static_cast<int32_t>(pos.z), 0, 0);

    epicentres.push_back(ent);
  }

  if (epicentres.empty()) return;

  // Everything that is going to be removed this tick, epicentres included.
  std::vector<entt::entity> toDestroy = epicentres;

  // gp-dd5q: blast wave. Sweep every machine the registry knows about and
  // clear the ones the epicentres reach, with a linear distance falloff.
  //
  // Visibility: the system has no chunk/block repository — IEventPublisher is
  // its only output channel — so the blast can only clear blocks that exist in
  // the ECS as MachineComponent + Position. Plain blocks and unloaded chunks are
  // unreachable. That is a real limitation, specified as such in
  // openspec/specs/explosion-mechanics/spec.md, not an accident.
  auto blastView = reg_.view<MachineComponent, Position>();
  for (auto ent : blastView) {
    // The epicentres are destroyed by the loop above; skip them so a single
    // explosion still publishes exactly one event for its own block.
    if (std::find(toDestroy.begin(), toDestroy.end(), ent) != toDestroy.end()) continue;

    const auto& machine = blastView.get<MachineComponent>(ent);
    const auto& pos = blastView.get<Position>(ent);

    // A blast victim is removed, not exploded: no fuse is evaluated for it, and
    // it is never added to `epicentres`, so the wave cannot relay outward one
    // machine at a time. The reach test below therefore always measures from a
    // real epicentre.
    bool caught = false;
    for (auto epicentre : epicentres) {
      const auto& e = blastView.get<Position>(epicentre);
      const int64_t d2 = squaredDistance(
          static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y),
          static_cast<int32_t>(pos.z), static_cast<int32_t>(e.x),
          static_cast<int32_t>(e.y), static_cast<int32_t>(e.z));
      const int32_t reach = reachFor(machine.mb_id != 0);
      if (d2 <= static_cast<int64_t>(reach) * reach) {
        caught = true;
        break;
      }
    }
    if (!caught) continue;

    spdlog::debug("[Explosion] blast caught machine at ({},{},{})", pos.x, pos.y, pos.z);
    events_->publishBlockChangedEvent(static_cast<int32_t>(pos.x),
                                      static_cast<int32_t>(pos.y),
                                      static_cast<int32_t>(pos.z), 0, 0);
    toDestroy.push_back(ent);
  }

  for (auto ent : toDestroy) {
    reg_.destroy(ent);
  }
}

} // namespace simcore
