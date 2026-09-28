#pragma once

#include "apps/simcore/Network/IEventPublisher.h"
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/ISystem.h>
#include <engine/sim/MachineRegistry.h>
#include "apps/simcore/Network/FluidClient.h"
#include <engine/registry/ItemId.h>
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class PipeEnergyClient;
class FluidClient;

class CreativeFluidSystem : public ISystem {
public:
  CreativeFluidSystem(entt::registry &reg,
                      std::shared_ptr<IEventPublisher> events,
                      std::shared_ptr<FluidClient> fluidClient);

  void tick(float dt) override;

  // Infinite OIL source (machines.yaml block_id 1110:100:15, name
  // creative_oil_generator). Publishes the `oil` fluid id, not a block id.
  static constexpr uint16_t kCreativeOilGeneratorBlockId =
      ItemId::pack("1110:100:15");
  // Infinite WATER source (machines.yaml block_id 1110:100:16, name
  // creative_water_generator). Publishes the `water` fluid id.
  static constexpr uint16_t kCreativeWaterGeneratorBlockId =
      ItemId::pack("1110:100:16");

  // Fluid ids from src/content/data/registry/fluids.csv, the SAME namespace
  // the network uses for fluid.node.update.fluid_id. NOT block ids:
  //   oil   -> 1111:11:58
  //   water -> 1111:11:0
  // Published verbatim into FluidClient::publishNodeUpdate's fluid_id, so a
  // mismatch here silently makes the source advertise a fluid nothing can pull.
  static constexpr uint32_t kOilFluidId = ItemId::pack("1111:11:58");
  static constexpr uint32_t kWaterFluidId = ItemId::pack("1111:11:0");

  // Default buffer sizing, used ONLY when the machine carries no FluidStorage
  // to copy from (see tick()). Matches the machines.yaml `energy:` block of
  // the creative generators (capacity 1000000, max_output 100000, tier 10), so
  // an invented value here is never published to the client as content.
  static constexpr int32_t kDefaultFluidCapacity = 1000000;
  static constexpr int32_t kDefaultFluidMaxOutput = 100000;
  static constexpr int32_t kDefaultTier = 10;

  // mB per tick a creative source produces. Infinite source: the buffer is
  // topped back up every tick, so this only sets the refill rate, never a
  // ceiling on total output.
  static constexpr int32_t kDefaultFluidPerTick = 100000;

  void setFluidPerTick(int32_t val) { fluidPerTick_ = val; }

  // Which fluid a creative block id produces, or 0 for any other machine.
  // Exposed for tests and for callers that need the mapping without a tick.
  static uint32_t fluidForBlock(uint16_t block_id);

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<FluidClient> fluidClient_;
  int32_t fluidPerTick_ = kDefaultFluidPerTick;
};

} // namespace simcore
