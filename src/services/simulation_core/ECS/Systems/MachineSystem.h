#pragma once

#include "../../Network/IEventPublisher.h"
#include "../../RecipeManager/RecipeManager.h"
#include "../components/EnergyStorage.h"
#include "../components/InventoryContainer.h"
#include "../components/MachineComponent.h"
#include "../components/RecipeProgress.h"
#include "ISystem.h"
#include "MachineRegistry.h"
#include <entt/entt.hpp>
#include <memory>
#include <spdlog/spdlog.h>
#include <unordered_map>

namespace simcore {

class PipeEnergyClient;
class ItemClient;
class ContainerSessionRegistry;
class PlayerInventoryStore;
class IoUringRouterClient;

class MachineSystem : public ISystem {
public:
  MachineSystem(entt::registry &reg,
                std::shared_ptr<RecipeManager::RecipeManager> recipes,
                std::shared_ptr<IEventPublisher> events,
                std::shared_ptr<PipeEnergyClient> pipeClient,
                std::shared_ptr<ItemClient> itemClient = nullptr,
                std::shared_ptr<ContainerSessionRegistry> sessions = nullptr,
                std::shared_ptr<PlayerInventoryStore> invStore = nullptr,
                std::shared_ptr<IoUringRouterClient> router = nullptr);

  void tick(float dt) override;
  void onConsumeResponse(uint64_t node_id = 0, int32_t consumed = 0,
                         int32_t remaining = 0);

  // TODO(perf): force-publishing every machine every 10 ticks (~0.5s) is
  // O(#machines) traffic per interval — temporary measure so late-connecting
  // clients catch up on machine state. Revisit (dirty-flag only) once machine
  // counts grow.
  static constexpr int kForcePublishInterval = 10;

private:
  void pushOutputToPipe(uint64_t entity_id, const MachineComponent& machine,
                        InventoryContainer& container, int slots_in);

  void publishInventoryIfOpen(const MachineComponent& mc);

  entt::registry &reg_;
  std::shared_ptr<RecipeManager::RecipeManager> recipes_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::shared_ptr<ItemClient> itemClient_;
  std::shared_ptr<ContainerSessionRegistry> sessions_;
  std::shared_ptr<PlayerInventoryStore> invStore_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::unordered_map<uint64_t, int32_t> pendingConsumes_;
  std::unordered_map<uint64_t, uint64_t> lastInventoryHash_;
  int tickCounter_ = 0;
  int startupTicks_ = 3;
};

} // namespace simcore
