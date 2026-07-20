#pragma once
#include "../Network/ITopicHandler.h"
#include <memory>
#include <vector>

namespace simcore {

class WrenchHandler;
class IoUringRouterClient;

class WrenchActionHandler : public ITopicHandler {
public:
  explicit WrenchActionHandler(std::shared_ptr<WrenchHandler> wrenchHandler);

  void handle(const std::vector<uint8_t>& data) override;
  void setRouter(std::shared_ptr<IoUringRouterClient> router) { router_ = std::move(router); }

private:
  std::shared_ptr<WrenchHandler> wrenchHandler_;
  std::shared_ptr<IoUringRouterClient> router_;
};

} // namespace simcore
