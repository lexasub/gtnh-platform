#pragma once

#include "../../Network/IEventPublisher.h"
#include "../../Storage/IBlockRepository.h"
#include "../components/DrillComponent.h"
#include "../components/EnergyStorage.h"
#include "ISystem.h"
#include "Network/PipeEnergyClient.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

namespace simcore {

class DrillSystem : public ISystem {
public:
  DrillSystem(entt::registry &reg, std::shared_ptr<IBlockRepository> blockRepo,
              std::shared_ptr<IEventPublisher> events,
              std::shared_ptr<PipeEnergyClient> pipeClient);

  void tick(float dt) override;

private:
  void phaseEnergyCheck(entt::entity ent, DrillComponent &drill,
                        EnergyStorage &energy);
  void phaseSearch(entt::entity ent, DrillComponent &drill);
  void phaseMine(entt::entity ent, DrillComponent &drill);

  static bool isOreBlock(uint16_t block_id);
  static uint16_t oreToDrop(uint16_t block_id);
  static void getSpiralOffset(int32_t n, int32_t &dx, int32_t &dz);

  void onSearchBlockResult(entt::entity ent, int32_t x, int32_t y, int32_t z,
                           const BlockData &block);
  void onMineComplete(entt::entity ent, const DrillComponent &drill,
                      const CASResult &result);

  // Member variables
  entt::registry &reg_;
  std::shared_ptr<IBlockRepository> blockRepo_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::unordered_map<entt::entity, int32_t> pendingSearches_;
};

} // namespace simcore