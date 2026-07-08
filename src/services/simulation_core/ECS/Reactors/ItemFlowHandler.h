#pragma once
#include "../../Network/ITopicHandler.h"
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class ItemClient;

class ItemFlowHandler : public ITopicHandler {
public:
  ItemFlowHandler(entt::registry &reg,
                  std::shared_ptr<ItemClient> itemClient);

  void handle(const std::vector<uint8_t> &data) override;

private:
  entt::registry &reg_;
  std::shared_ptr<ItemClient> itemClient_;
};

} // namespace simcore
