#pragma once
#include "../Network/ITopicHandler.h"
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
                               std::shared_ptr<QuestManager> questManager);

  void handle(const std::vector<uint8_t>& data) override;
  void setRouter(std::shared_ptr<IoUringRouterClient> router) { router_ = std::move(router); }

private:
  // Cooldown: key = hash(playerId, x, y, z, face) → last action timepoint
  static uint64_t cooldownKey(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face);
  static constexpr auto COOLDOWN_MS = std::chrono::milliseconds(200);

  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastActionTime_;
  std::shared_ptr<WrenchHandler> wrenchHandler_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::shared_ptr<QuestManager> questManager_;
};

} // namespace simcore
