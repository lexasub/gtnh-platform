#pragma once
#include "Actions/ActionDispatcher.h"
#include "Actions/Callbacks.h"
#include "core_generated.h"
#include <memory>

namespace simcore {

class IBlockRepository;
class IEventPublisher;
class SimulationEngine;
class PlayerInventoryStore;
class EntityStateStoreClient;

// Thin facade: owns the deps, builds an ActionContext per incoming
// SetBlockAction and routes it through ActionDispatcher (machine → chest →
// break → place). All behavior lives in the handlers.
class SetBlockCASHandler {
public:
  SetBlockCASHandler(std::shared_ptr<IBlockRepository> repo,
                     std::shared_ptr<IEventPublisher> publisher,
                     std::shared_ptr<SimulationEngine> engine,
                     std::shared_ptr<PlayerInventoryStore> inventoryStore = nullptr,
                     ItemGiveCallback onGiveItem = nullptr,
                     DrillUseCallback onDrillUse = nullptr,
                     BlockPlacedCallback onBlockPlaced = nullptr,
                     PostCallback postToMain = nullptr);

  // Public entry point used by SimCoreMessageHandler (and tests):
  // table must be a Protocol::SetBlockAction.
  void handle(const void* table);

  void setEntityStateStore(std::shared_ptr<EntityStateStoreClient> client) {
    entityStateClient_ = std::move(client);
  }

private:
  void handle(const Protocol::SetBlockAction* action);

  std::shared_ptr<IBlockRepository> repo_;
  std::shared_ptr<IEventPublisher> publisher_;
  std::shared_ptr<SimulationEngine> engine_;
  std::shared_ptr<PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<EntityStateStoreClient> entityStateClient_;
  ItemGiveCallback onGiveItem_;
  DrillUseCallback onDrillUse_;
  BlockPlacedCallback onBlockPlaced_;
  PostCallback postToMain_;
  ActionDispatcher dispatcher_;
};

} // namespace simcore
