#include "CreativeFluidSystem.h"
#include <engine/sim/components/FluidStorage.h>
#include <spdlog/spdlog.h>

namespace simcore {

namespace {

// The creative fluid blocks and the fluid each one produces. A TABLE, not one
// hardcoded fluid: machines.yaml declares two independent sources
// (creative_oil_generator 1110:100:15, creative_water_generator 1110:100:16),
// and the fluid id in the wire message is the only thing telling PipeNetwork
// what a source actually emits. Collapsing these to a single constant would
// make the water block emit oil — no error anywhere, just a machine quietly
// fed the wrong fluid.
struct CreativeFluidSource {
  uint16_t block_id;
  uint32_t fluid_id;
};

constexpr CreativeFluidSource kCreativeFluidSources[] = {
    {CreativeFluidSystem::kCreativeOilGeneratorBlockId,
     CreativeFluidSystem::kOilFluidId},
    {CreativeFluidSystem::kCreativeWaterGeneratorBlockId,
     CreativeFluidSystem::kWaterFluidId},
};

} // namespace

uint32_t CreativeFluidSystem::fluidForBlock(uint16_t block_id) {
  for (const auto& src : kCreativeFluidSources) {
    if (src.block_id == block_id) return src.fluid_id;
  }
  return 0;
}

CreativeFluidSystem::CreativeFluidSystem(entt::registry& reg,
                                          std::shared_ptr<IEventPublisher> events,
                                          std::shared_ptr<FluidClient> fluidClient)
    : reg_(reg), events_(events), fluidClient_(fluidClient)
{
}

void CreativeFluidSystem::tick(float /*dt*/) {
    // MachineComponent only. EnergyStorage is deliberately NOT required: these
    // blocks produce fluid, not EU, and machines.yaml gives them an `energy:`
    // block purely so the engine creates a buffer. Requiring it here would make
    // a fluid source depend on an energy field it never uses, and would skip
    // the block outright if that field were ever dropped from content.
    auto view = reg_.view<MachineComponent>();

    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);

        const uint32_t fluid_id = fluidForBlock(machine.machine_id);
        if (fluid_id == 0) continue;   // not a creative fluid source

        // try_get + emplace, never a filtered view on FluidStorage: the buffer
        // is created lazily. machines.yaml declares no `fluid:` section for
        // these blocks and SimulationEngine.cpp only ever emplaces FluidStorage
        // for recipe fluid outputs, so a creative block arrives with none. A
        // view filtered on FluidStorage would simply never visit the entity and
        // the source would be permanently silent. Emplacing it is safe because
        // the machine_id check above has already proven this is our block, and
        // it is created once — subsequent ticks take the try_get branch.
        auto* fluid = reg_.try_get<FluidStorage>(ent);
        if (!fluid) {
            fluid = &reg_.emplace<FluidStorage>(
                ent, fluid_id, /*amount=*/0, kDefaultFluidCapacity,
                /*maxInput=*/0, kDefaultFluidMaxOutput);
        }

        // A buffer holding a different fluid is not ours to overwrite — it may
        // be a legitimate recipe output. Leave it alone rather than corrupt it.
        if (fluid->fluid_id != 0 && fluid->fluid_id != fluid_id) {
            spdlog::warn("Creative fluid source entity {} holds fluid {} but block {} produces {}",
                         static_cast<uint32_t>(ent), fluid->fluid_id,
                         machine.machine_id, fluid_id);
            continue;
        }
        if (fluid->fluid_id == 0) fluid->fluid_id = fluid_id;

        // Refill BEFORE the isFull() test, for the same reason
        // CreativeGeneratorSystem does: a buffer whose capacity is 0 is
        // trivially "full" (amount >= capacity), so checking first would leave
        // such a machine permanently idle and invisible. The clamp to headroom
        // below is what actually prevents overflow, and it runs on every tick.
        int32_t space = fluid->capacity - fluid->amount;
        int32_t toAdd = (fluidPerTick_ < space) ? fluidPerTick_ : space;
        if (toAdd < 0) toAdd = 0;
        if (toAdd > 0) fluid->amount += toAdd;

        // Publish every tick, including on a full buffer. Downstream consumers
        // are the reason this node exists: a full creative source is a working
        // infinite source, and skipping the publish would let PipeNetwork drop
        // the node and silently unhook every consumer.
        if (fluidClient_) {
            fluidClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                static_cast<int32_t>(machine.x),
                static_cast<int32_t>(machine.y),
                static_cast<int32_t>(machine.z),
                fluid->fluid_id,
                fluid->amount,
                fluid->capacity,
                fluid->maxInput,
                fluid->maxOutput,
                kDefaultTier,
                true,   // is_source — downstream machines pull from this node
                false   // is_sink
            );
        }

        if (events_) {
            events_->publishBlockEntityUpdate(
                machine.x, machine.y, machine.z,
                machine.machine_id,
                {},
                1.0f,
                static_cast<uint32_t>(fluid->amount));
        }
    }
}

} // namespace simcore
