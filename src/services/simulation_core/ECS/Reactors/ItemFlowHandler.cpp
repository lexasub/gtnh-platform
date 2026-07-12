#include "ItemFlowHandler.h"
#include "Network/ItemClient.h"
#include "ECS/components/Position.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/InventoryContainer.h"
#include "pipe_network_generated.h"
#include <spdlog/spdlog.h>

#include "MachineRegistry.h"

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

    auto view = reg_.view<const Position, const MachineComponent, InventoryContainer>();
    for (auto entity : view) {
        auto& pos = view.get<const Position>(entity);
        auto& machine = view.get<const MachineComponent>(entity);
        auto& container = view.get<InventoryContainer>(entity);
        
        if (static_cast<int32_t>(pos.x) == x &&
            static_cast<int32_t>(pos.y) == y &&
            static_cast<int32_t>(pos.z) == z) {
            // Check if this is a machine and if the face allows input
            // Determine which face the pipe approaches from
            // For now, assume the machine is at the same position as the sink node
            // We need to determine the face from the pipe's perspective
            
            // Get input slot count from MachineRegistry
            int slots_in = static_cast<int>(container.slots.size());
            if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id)) {
                slots_in = minfo->slots_in;
            }
            
            // Check if there are input slots available
            if (slots_in <= 0) {
                spdlog::debug("ItemFlowHandler: machine at ({},{},{}) has no input slots, item lost", x, y, z);
                break;
            }
            
            // For now, deliver to first available input slot
            // TODO: Determine which face the pipe approaches from and check side_config
            // For a complete implementation, we would need:
            // 1. The pipe's position to determine which face we're approaching from
            // 2. Check machine.side_config[face] for INPUT or ANY
            // 3. Only deliver if the face allows input
            
            // Simple implementation: deliver to first available input slot
            bool delivered = false;
            for (int i = 0; i < slots_in && !delivered; ++i) {
                if (container.slots[i].item_id == 0) {
                    container.slots[i] = {item_id, static_cast<uint8_t>(count), 0};
                    delivered = true;
                    spdlog::debug("ItemFlowHandler: item {} x{} delivered to machine input slot {} at ({},{},{})",
                                  item_id, count, i, x, y, z);
                    break;
                }
            }
            
            if (!delivered) {
                // Try to stack with existing item
                for (int i = 0; i < slots_in && !delivered; ++i) {
                    if (container.slots[i].item_id == item_id && container.slots[i].count < 64) {
                        uint8_t space = 64 - container.slots[i].count;
                        uint8_t add = std::min(static_cast<uint8_t>(count), space);
                        container.slots[i].count += add;
                        spdlog::debug("ItemFlowHandler: item {} x{} stacked into machine input slot {} at ({},{},{})",
                                      item_id, add, i, x, y, z);
                        delivered = true;
                        break;
                    }
                }
            }
            
            if (!delivered) {
                spdlog::debug("ItemFlowHandler: machine input slots full at ({},{},{}), item lost", x, y, z);
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
