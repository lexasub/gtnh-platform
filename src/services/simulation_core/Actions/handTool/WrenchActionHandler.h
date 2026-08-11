#pragma once
#include "../../Network/ITopicHandler.h"
#include "Storage/IBlockRepository.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace simcore {

class WrenchHandler;
class IoUringRouterClient;
class QuestManager;

class WrenchActionHandler : public ITopicHandler {
public:
  explicit WrenchActionHandler(std::shared_ptr<WrenchHandler> wrenchHandler,
                               std::shared_ptr<QuestManager> questManager,
                               std::shared_ptr<IBlockRepository> blockRepository);

  void handle(const std::vector<uint8_t>& data) override;
  void setRouter(std::shared_ptr<IoUringRouterClient> router) { router_ = std::move(router); }

private:
  // Cooldown: key = hash(playerId, x, y, z, face) → last action timepoint
  static uint64_t cooldownKey(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face);
  static constexpr auto COOLDOWN_MS = std::chrono::milliseconds(200);

  // Re-publish a block (id+meta) so the client mesh and PipeNetwork pick up a
  // meta-only change. setBlockCAS writes the store but does NOT emit this event.
  // source_player_id is left 0 (NOT the acting player) on purpose: the wrench
  // client receives no BlockAck carrying the new meta, so it MUST receive this
  // BlockChangedEvent. The gateway drops world.blocks.changed when
  // source_player_id == the connected client's id (it assumes an optimistic
  // BlockAck already applied) — which would silently hide the toggle for the
  // local player. source_player_id == 0 disables that skip for all clients.
  void publishBlockChanged(int32_t x, int32_t y, int32_t z,
                           uint16_t block_id, uint8_t meta);

  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastActionTime_;
  std::shared_ptr<WrenchHandler> wrenchHandler_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::shared_ptr<QuestManager> questManager_;
  std::shared_ptr<IBlockRepository> blockRepository_;
};

} // namespace simcore
