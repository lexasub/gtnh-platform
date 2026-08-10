#pragma once
#include "../../Network/ITopicHandler.h"
#include <memory>
#include <vector>

namespace simcore {

class IoUringRouterClient;

// Answers PipeNetwork's `pipe.wrench.response` by publishing a player-facing
// `ToolActionResp` with the human-readable guidance message.
class PipeWrenchResponseHandler : public ITopicHandler {
public:
  explicit PipeWrenchResponseHandler(std::shared_ptr<IoUringRouterClient> router);
  void handle(const std::vector<uint8_t>& data) override;

private:
  std::shared_ptr<IoUringRouterClient> router_;
};

} // namespace simcore
