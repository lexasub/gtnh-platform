#include "MachineSlotHandler.h"
#include "Quest/QuestManager.h"
#include "ECS/SimulationEngine.h"
#include "ECS/components/Position.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/InventoryContainer.h"
#include "Storage/PlayerInventoryStore.h"
#include "Storage/ChunkStoreRepository.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "Network/IEventPublisher.h"
#include "Network/clients/IoUringRouterClient.h"
#include "machine_state_generated.h"
#include "MachineRegistry.h"
#include <spdlog/spdlog.h>
#include <cstring>
namespace simcore {
MachineSlotHandler::MachineSlotHandler(std::shared_ptr<SimulationEngine> e, std::shared_ptr<PlayerInventoryStore> inv,
    std::shared_ptr<EntityStateStoreClient> ess, std::shared_ptr<IEventPublisher> ev, std::shared_ptr<IoUringRouterClient> r,
    std::shared_ptr<ChunkStoreRepository> csr, std::shared_ptr<QuestManager> qm)
    : engine_(std::move(e)), inventoryStore_(std::move(inv)), entityState_(std::move(ess)), events_(std::move(ev)), router_(std::move(r)), chunkStore_(std::move(csr)), questManager_(std::move(qm)) {}
void MachineSlotHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::SetMachineSlotReq>(nullptr)) return;
    auto* req = flatbuffers::GetRoot<Protocol::SetMachineSlotReq>(data.data());
    if (!req || !req->pos()) return;
    auto* p = req->pos();
    uint32_t x = p->x(), y = p->y(), z = p->z();
    uint16_t slot_idx = req->slot_index(), item_id = req->item_id();
    uint8_t count = req->count(), ps = req->player_slot();
    auto pub = [&](bool ok, const char* err = nullptr) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto pos = Protocol::Vec3i(static_cast<int32_t>(x),static_cast<int32_t>(y),static_cast<int32_t>(z));
        auto e = err ? fbb.CreateString(err) : 0;
        auto resp = Protocol::CreateSetMachineSlotResp(fbb, &pos, static_cast<uint8_t>(slot_idx), ok, e);
        fbb.Finish(resp); std::vector<uint8_t> rd(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        router_->Publish("player.machine.slot.response", std::move(rd));
    };
    auto& reg = engine_->reg(); entt::entity entity = entt::null;
    auto vw = reg.view<const Position>();
    for (auto e : vw) { auto& pp = vw.get<const Position>(e);
        if (static_cast<int32_t>(pp.x)==static_cast<int32_t>(x) && static_cast<int32_t>(pp.y)==static_cast<int32_t>(y) && static_cast<int32_t>(pp.z)==static_cast<int32_t>(z)) { entity = e; break; } }
    if (entity == entt::null) {
        spdlog::warn("[MachineSlotHandler] no ECS entity at ({},{},{}) — lazy-init from ChunkStore", x, y, z);
        if (chunkStore_) {
            chunkStore_->getBlock(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z),
                [engine = engine_, rx = x, ry = y, rz = z](const BlockData& bd) {
                    if (bd.block_id != 0) {
                        engine->onBlockChanged(rx, ry, rz, bd.block_id, bd.meta, bd.mb_id);
                        spdlog::info("[SimCore] Lazy-created ECS entity at ({},{},{}) block_id={}", rx, ry, rz, bd.block_id);
                    }
                });
        }
        pub(false,"No machine");
        return;
    }
    auto* container = reg.try_get<InventoryContainer>(entity);
    if (!container || slot_idx >= container->slots.size()) {
        spdlog::warn("[SimCore] MachineSlotHandler: invalid slot {} at ({},{},{}) slots={}",
                     slot_idx, x, y, z, container ? container->slots.size() : 0);
        pub(false,"Invalid slot");
        return;
    }
    InventorySlot oldItem = container->slots[slot_idx];
    container->slots[slot_idx] = {item_id, count, 0};
    uint16_t machineId = 0;
    if (auto* mc = reg.try_get<MachineComponent>(entity)) {
        machineId = mc->machine_id;
    }
    spdlog::info("[SimCore] MachineSlotHandler: slot {} at ({},{},{}) {}→{} ({}x), ps={}",
                 slot_idx, x, y, z, oldItem.item_id, item_id, count, ps);
    if (ps < 255) {
        auto inv = inventoryStore_->getSlots(req->player_id());
        // ps may be a machine-slot source id (>= 200) from machine-to-machine
        // drag — skip player inventory interaction when out of bounds.
        if (ps >= inv.size()) {
            spdlog::warn("[SimCore] MachineSlotHandler: player_slot {} >= inv.size() {} — "
                         "treating as no-slot (machine-to-machine drag)", ps, inv.size());
            goto skip_player_inv;
        }
        spdlog::info("[SimCore] MachineSlotHandler: player {} slot {} was item {}",
                     req->player_id(), ps, inv[ps].item_id);
        if (item_id != 0) {
            // Placing item into machine — consume from player inventory
            if (oldItem.item_id != 0) {
                inv[ps] = {oldItem.item_id, oldItem.count, 0};
                spdlog::info("[SimCore] MachineSlotHandler: swapped old machine item {} into player slot {}",
                             oldItem.item_id, ps);
            } else {
                inv[ps] = {};
                spdlog::info("[SimCore] MachineSlotHandler: cleared player slot {}", ps);
            }
        } else if (oldItem.item_id != 0) {
            // Taking item from machine — give to player
            inv[ps] = {oldItem.item_id, oldItem.count, 0};
            spdlog::info("[SimCore] MachineSlotHandler: gave machine item {} to player slot {}",
                         oldItem.item_id, ps);
            if (questManager_ && machineId != 0) {
                questManager_->checkMachineOutput(req->player_id(), machineId,
                                                  oldItem.item_id, oldItem.count);
            }
        }
        inventoryStore_->setSlots(req->player_id(), inv);
    }
skip_player_inv:
    int slotsIn = static_cast<int>(container->slot_count);
    if (auto* mc = reg.try_get<MachineComponent>(entity)) {
        machineId = mc->machine_id;
        if (auto* info = MachineRegistry::instance()->Get(mc->machine_id)) {
            slotsIn = info->slots_in;
        }
        flatbuffers::FlatBufferBuilder fbb(256);
        std::vector<flatbuffers::Offset<Protocol::MachineInventorySlot>> offs;
        for (auto& s : container->slots) offs.push_back(Protocol::CreateMachineInventorySlot(fbb, s.item_id, s.count, s.meta));
        auto inv = Protocol::CreateMachineInventory(fbb, container->slot_count, fbb.CreateVector(offs));
        auto st = Protocol::CreateMachineState(fbb, 1, nullptr, 0, inv, 0);
        fbb.Finish(st); std::vector<uint8_t> blob(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        entityState_->SaveEntityState(0, mc->x, mc->y, mc->z, mc->machine_id, blob, [](bool){});
    }
    std::vector<uint8_t> rawInv(container->slots.size() * 5);
    {
        uint8_t* ptr = rawInv.data();
        for (auto& s : container->slots) {
            std::memcpy(ptr, &s.item_id, sizeof(uint16_t)); ptr += sizeof(uint16_t);
            *ptr++ = s.count;
            std::memcpy(ptr, &s.meta, sizeof(uint16_t)); ptr += sizeof(uint16_t);
        }
    }
    EnergyType etype = EnergyType::ELECTRICITY;
    if (auto* es = reg.try_get<EnergyStorage>(entity)) {
        etype = es->type;
    }
    events_->publishBlockEntityUpdate(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z),
                                      machineId, rawInv, 0.0f, 0, etype, 0, slotsIn);
    pub(true);
    spdlog::info("[SimCore] Set slot {} at ({},{},{}) to item {} count {} — OK", slot_idx, x, y, z, item_id, count);
}
} // namespace simcore
