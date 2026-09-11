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
class FluidClient;
class ItemClient;
class ContainerSessionRegistry;
class PlayerInventoryStore;
class IoUringRouterClient;
class CraftReservationClient;

class MachineSystem : public ISystem {
public:
  MachineSystem(entt::registry &reg,
                std::shared_ptr<RecipeManager::RecipeManager> recipes,
                std::shared_ptr<IEventPublisher> events,
                std::shared_ptr<PipeEnergyClient> pipeClient,
                std::shared_ptr<ItemClient> itemClient = nullptr,
                std::shared_ptr<ContainerSessionRegistry> sessions = nullptr,
                std::shared_ptr<PlayerInventoryStore> invStore = nullptr,
                std::shared_ptr<IoUringRouterClient> router = nullptr,
                std::shared_ptr<FluidClient> fluidClient = nullptr,
                std::shared_ptr<CraftReservationClient> reservations = nullptr,
                std::uint16_t steam_item_id = 0);

  void tick(float dt) override;
  void onConsumeResponse(uint64_t node_id = 0, int32_t consumed = 0,
                         int32_t remaining = 0);
  void onFluidConsumeResponse(int32_t consumed);

  // Commit a fully-accepted pending craft (4.3.2): consumes the input items
  // and starts progress exactly once. No-op unless the entity carries a
  // PendingCraft whose reservations are all accepted.
  void commitPendingCraft(entt::entity entity);

  // TODO(perf): force-publishing every machine every 10 ticks (~0.5s) is
  // O(#machines) traffic per interval — temporary measure so late-connecting
  // clients catch up on machine state. Revisit (dirty-flag only) once machine
  // counts grow.
  static constexpr int kForcePublishInterval = 10;

  // Passive steam top-up: max amount requested per delivery when a steam
  // machine's tank is below capacity. Matches the pipe buffer capacity and
  // the solid/heat boiler output rate, so a 10000-tank fills in ~10
  // deliveries. PipeNetwork short-fills when the network has less.
  static constexpr int32_t kSteamFillQuantum = 1000;

private:
  void pushOutputToPipe(uint64_t entity_id, const MachineComponent& machine,
                        InventoryContainer& container, int slots_in);

  void publishInventoryIfOpen(const MachineComponent& mc);

  // Reservation-driven recipe start (4.2.2/4.3.3): begin/tick/commit the
  // pending craft for `recipe` on an idle machine.
  void tickOrchestratedStart(entt::entity entity, MachineComponent& machine,
                             const RecipeManager::Recipe& recipe);

  entt::registry &reg_;
  std::shared_ptr<RecipeManager::RecipeManager> recipes_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::shared_ptr<ItemClient> itemClient_;
  std::shared_ptr<ContainerSessionRegistry> sessions_;
  std::shared_ptr<PlayerInventoryStore> invStore_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::shared_ptr<FluidClient> fluidClient_;
  std::shared_ptr<CraftReservationClient> reservations_;
  // Registry-resolved Steam item id; 0 (registry unavailable) fails closed:
  // the legacy steam path never requests steam, so the recipe stays pending.
  std::uint16_t steam_item_id_ = 0;
  std::unordered_map<uint64_t, int32_t> pendingConsumes_;
  std::unordered_map<uint64_t, int32_t> pendingFluidConsumes_;
  std::unordered_map<uint64_t, uint64_t> lastInventoryHash_;
  int tickCounter_ = 0;
  int startupTicks_ = 3;
  uint64_t totalTicks_ = 0;
};

} // namespace simcore
