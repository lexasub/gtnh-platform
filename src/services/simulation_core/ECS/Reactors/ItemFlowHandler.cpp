#include "ItemFlowHandler.h"
#include "Network/ItemClient.h"
#include "ECS/components/Position.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/InventoryContainer.h"
#include "pipe_network_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

ItemFlowHandler::ItemFlowHandler(entt::registry& reg,
                                  std::shared_ptr<ItemClient> itemClient)
    : reg_(reg), itemClient_(std::move(itemClient))
{}

void ItemFlowHandler::handle(const std::vector<uint8_t>& data) {
    auto* flow = flatbuffers::GetRoot<Protocol::ItemFlowEvent>(data.data());
    if (!flow || !flow->pos()) return;

    int32_t x = flow->pos()->x();
    int32_t y = flow->pos()->y();
    int32_t z = flow->pos()->z();
    uint64_t from_node = flow->from_node_id();
    uint16_t item_id = flow->item_id();
    int8_t count = flow->count();
    if (from_node == 0 || item_id == 0 || count <= 0) return;

    auto view = reg_.view<const Position>();
    for (auto entity : view) {
        auto& pos = view.get<const Position>(entity);
        if (static_cast<int32_t>(pos.x) == x &&
            static_cast<int32_t>(pos.y) == y &&
            static_cast<int32_t>(pos.z) == z) {
            if (auto* container = reg_.try_get<InventoryContainer>(entity)) {
                container->addItem(item_id, static_cast<uint8_t>(count), 0);
                spdlog::debug("ItemFlowHandler: item {} x{} delivered to machine at ({},{},{})",
                              item_id, count, x, y, z);
            }

            if (itemClient_) {
                itemClient_->publishNodeUpdate(
                    from_node, x, y, z,
                    {}, {}, 0, false, false);
            }
            break;
        }
    }
}

} // namespace simcore
