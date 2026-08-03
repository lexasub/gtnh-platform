#include "ItemFlowHandler.h"
#include "ECS/components/Position.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/InventoryContainer.h"
#include "pipe_network_generated.h"
#include "machine_state_generated.h"
#include "MachineRegistry.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace simcore {

ItemFlowHandler::ItemFlowHandler(entt::registry& reg,
                                  std::shared_ptr<ItemClient> itemClient,
                                  std::shared_ptr<IoUringRouterClient> router,
                                  std::shared_ptr<EntityStateStoreClient> entityState)
    : reg_(reg), itemClient_(std::move(itemClient)),
      router_(std::move(router)), entityState_(std::move(entityState))
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

        if (static_cast<int32_t>(pos.x) != x ||
            static_cast<int32_t>(pos.y) != y ||
            static_cast<int32_t>(pos.z) != z)
            continue;

        // Check side_config: at least one face must allow INPUT or be unset (NONE)
        bool faceAllowsInput = false;
        for (int f = 0; f < 6; ++f) {
            uint8_t role = machine.getFaceRole(f);
            if (role == static_cast<uint8_t>(MachineFaceRole::INPUT) ||
                role == static_cast<uint8_t>(MachineFaceRole::NONE)) {
                faceAllowsInput = true;
                break;
            }
        }
        if (!faceAllowsInput) {
            spdlog::debug("ItemFlowHandler: machine at ({},{},{}) has no INPUT face, item blocked", x, y, z);
            break;
        }

        // Get input slot count from MachineRegistry
        int slots_in = static_cast<int>(container.slots.size());
        if (auto* minfo = MachineRegistry::instance()->Get(machine.machine_id)) {
            slots_in = minfo->slots_in;
        }

        if (slots_in <= 0) {
            spdlog::debug("ItemFlowHandler: machine at ({},{},{}) has no input slots", x, y, z);
            break;
        }

        // Deliver to first available input slot
        bool delivered = false;
        int slot_idx = -1;
        for (int i = 0; i < slots_in && !delivered; ++i) {
            if (container.slots[i].item_id == 0) {
                container.slots[i] = {item_id, static_cast<uint8_t>(count), 0};
                slot_idx = i;
                delivered = true;
                break;
            }
        }

        if (!delivered) {
            for (int i = 0; i < slots_in && !delivered; ++i) {
                if (container.slots[i].item_id == item_id && container.slots[i].count < 64) {
                    uint8_t space = 64 - container.slots[i].count;
                    uint8_t add = (count < space) ? static_cast<uint8_t>(count) : space;
                    container.slots[i].count += add;
                    slot_idx = i;
                    delivered = true;
                    break;
                }
            }
        }

        if (!delivered) {
            spdlog::debug("ItemFlowHandler: machine input slots full at ({},{},{})", x, y, z);
            break;
        }

        // Send SetMachineSlotReq to persist the inventory change
        if (router_) {
            flatbuffers::FlatBufferBuilder fbb(128);
            auto pos_fb = Protocol::Vec3i(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z));
            auto req = Protocol::CreateSetMachineSlotReq(
                fbb, 0, &pos_fb,
                static_cast<uint8_t>(slot_idx), item_id,
                static_cast<uint8_t>(count), 255);
            fbb.Finish(req);
            std::vector<uint8_t> rd(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
            router_->Publish("player.machine.slot", std::move(rd));
        }

        // Save machine inventory to EntityStateStore
        if (entityState_) {
            flatbuffers::FlatBufferBuilder fbb(256);
            std::vector<flatbuffers::Offset<Protocol::MachineInventorySlot>> offs;
            for (auto& s : container.slots) {
                offs.push_back(Protocol::CreateMachineInventorySlot(fbb, s.item_id, s.count, s.meta));
            }
            auto inv = Protocol::CreateMachineInventory(fbb, container.slot_count, fbb.CreateVector(offs));
            auto st = Protocol::CreateMachineState(fbb, 1, nullptr, 0, inv, 0);
            fbb.Finish(st);
            std::vector<uint8_t> blob(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
            entityState_->SaveEntityState(0, machine.x, machine.y, machine.z, machine.machine_id, blob, [](bool){});

            std::vector<uint8_t> rawInv(container.slots.size() * 5);
            {
                uint8_t* ptr = rawInv.data();
                for (auto& s : container.slots) {
                    std::memcpy(ptr, &s.item_id, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                    *ptr++ = s.count;
                    std::memcpy(ptr, &s.meta, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                }
            }
        }

        spdlog::debug("ItemFlowHandler: item {} x{} delivered to machine slot {} at ({},{},{})",
                      item_id, count, slot_idx, x, y, z);

        if (itemClient_) {
            itemClient_->publishNodeUpdate(
                from_node, x, y, z,
                {}, {}, 0, false, false);
        }

        break;
    }
}

} // namespace simcore
