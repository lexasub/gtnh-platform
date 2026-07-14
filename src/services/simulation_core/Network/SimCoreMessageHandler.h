#pragma once
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace simcore {

class MainThreadQueue;
class SimulationEngine;
class IoUringRouterClient;
class RouterEventPublisher;
class PipeEnergyClient;
class FluidClient;
class ItemClient;
class PlayerInventoryStore;
class EntityStateStoreClient;
class ChunkStoreRepository;
class TopicDispatcher;
class ActionDispatcher;
class SetBlockCASHandler;
class ChunkEventHandler;
class WrenchHandler;
class WorldContainerInventory;
class MachineSystem;
class BatteryBufferSystem;

} // namespace simcore

namespace RecipeManager { class RecipeManager; }

namespace simcore {

class SimCoreMessageHandler {
public:
  struct Deps {
    MainThreadQueue* mainQueue = nullptr;
    std::shared_ptr<SimulationEngine> engine;
    std::shared_ptr<IoUringRouterClient> routerClient;
    std::shared_ptr<RouterEventPublisher> eventPublisher;
    std::shared_ptr<PipeEnergyClient> pipeEnergyClient;
    std::shared_ptr<FluidClient> fluidClient;
    std::shared_ptr<ItemClient> itemClient;
    std::shared_ptr<PlayerInventoryStore> inventoryStore;
    std::shared_ptr<EntityStateStoreClient> entityStateClient;
    std::shared_ptr<RecipeManager::RecipeManager> recipeManager;
    std::shared_ptr<ChunkStoreRepository> blockRepository;
    std::shared_ptr<WrenchHandler> wrenchHandler;
    MachineSystem* machineSystem = nullptr;
    BatteryBufferSystem* batteryBuffer = nullptr;
  };

  explicit SimCoreMessageHandler(Deps deps);
  void setup();
  void wireOnMessage(WorldContainerInventory& worldContainers);

private:
  Deps deps_;
  std::shared_ptr<TopicDispatcher> topicDispatcher_;
  std::shared_ptr<ActionDispatcher> dispatcher_;
  std::shared_ptr<SetBlockCASHandler> casHandler_;
  std::shared_ptr<ChunkEventHandler> chunkHandler_;
};

} // namespace simcore
