#pragma once
#include "../../Network/ItemClient.h"
#include "../../Network/ITopicHandler.h"
#include "../../Network/clients/IoUringRouterClient.h"
#include "../../Network/clients/EntityStateStoreClient.h"
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class ItemFlowHandler : public ITopicHandler {
public:
  ItemFlowHandler(entt::registry &reg,
                  std::shared_ptr<ItemClient> itemClient,
                  std::shared_ptr<IoUringRouterClient> router,
                  std::shared_ptr<EntityStateStoreClient> entityState);

  void handle(const std::vector<uint8_t> &data) override;

private:
  entt::registry &reg_;
  std::shared_ptr<ItemClient> itemClient_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::shared_ptr<EntityStateStoreClient> entityState_;
};

} // namespace simcore
