#include "WrenchActionHandler.h"
#include "WrenchMeta.h"
#include "WrenchHandler.h"
#include "Quest/QuestManager.h"
#include "Network/clients/IoUringRouterClient.h"
#include "Storage/IBlockRepository.h"
#include <common/ItemId.h>
#include "core_generated.h"
#include "pipe_network_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
WrenchActionHandler::WrenchActionHandler(std::shared_ptr<WrenchHandler> wrenchHandler,
                                         std::shared_ptr<QuestManager> questManager,
                                         std::shared_ptr<IBlockRepository> blockRepository)
    : wrenchHandler_(std::move(wrenchHandler)), questManager_(std::move(questManager)),
      blockRepository_(std::move(blockRepository)) {}

uint64_t WrenchActionHandler::cooldownKey(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face) {
    // Pack playerId (48 bits) + pos (16 bits each) + face (4 bits) into uint64
    // playerId is typically small (dev ID = 1), so 48 bits is safe.
    // Each coordinate is limited to ±2M blocks in practice, so 16 bits won't lose
    // precision for the cooldown purpose (same position ±32 blocks).
    uint64_t key = (playerId & 0xFFFFFFFFFFFFULL) << 16;
    key ^= (static_cast<uint64_t>(static_cast<int16_t>(x)) & 0xFFFF) << 48;
    key ^= (static_cast<uint64_t>(static_cast<int16_t>(y)) & 0xFFFF) << 32;
    key ^= (static_cast<uint64_t>(static_cast<int16_t>(z)) & 0xFFFF) << 16;
    key ^= static_cast<uint64_t>(face & 0xF) << 12;
    return key;
}

void WrenchActionHandler::publishBlockChanged(int32_t x, int32_t y, int32_t z,
                                             uint16_t block_id, uint8_t meta) {
    flatbuffers::FlatBufferBuilder builder(128);
    auto pos = Protocol::Vec3i(x, y, z);
    // source_player_id == 0 → gateway relays to every client, incl. the actor
    // (the wrench client has no BlockAck and depends on this event).
    auto event = Protocol::CreateBlockChangedEvent(builder, &pos, block_id, meta, 0, 0, 0);
    builder.Finish(event);
    std::vector<uint8_t> eventData(builder.GetBufferPointer(),
                                   builder.GetBufferPointer() + builder.GetSize());
    router_->Publish("world.blocks.changed", std::move(eventData));
}

void WrenchActionHandler::handle(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::ToolAction>(nullptr)) return;
    auto* action = flatbuffers::GetRoot<Protocol::ToolAction>(data.data());
    if (!action || !action->pos()) return;
    auto* p = action->pos();

    if (action->action() != Protocol::ToolActionType_WRENCH_CYCLE) return;

    uint64_t ckey = cooldownKey(action->player_id(), p->x(), p->y(), p->z(), action->face());
    auto now = std::chrono::steady_clock::now();
    auto it = lastActionTime_.find(ckey);
    if (it != lastActionTime_.end() && (now - it->second) < COOLDOWN_MS) {
        return;
    }
    lastActionTime_[ckey] = now;

    auto r = wrenchHandler_->cycleFace(action->player_id(), p->x(), p->y(), p->z(), action->face());

    // SIDE_CONFIGURED detection: a machine face was cycled successfully.
    // Hatches carry machine_id == 0 and are not side-config quest targets.
    if (r.success) {
        if (r.machine_id != 0 && questManager_) {
            questManager_->checkSideConfigured(action->player_id(), r.machine_id);
        }
        flatbuffers::FlatBufferBuilder fbb(128);
        auto err = r.error.empty() ? 0 : fbb.CreateString(r.error);
        auto roles = fbb.CreateVector(r.allRoles, 6);
        auto resp = Protocol::CreateToolActionResp(fbb, r.success, err, 0, 0, r.newRole, roles, 0);
        fbb.Finish(resp);
        std::vector<uint8_t> respData(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        router_->Publish("player.tool.action.response", std::move(respData));
        return;
    }

    // cycleFace already did the ECS machine lookup. For a pipe/cable target the
    // wrench toggles the connection on the clicked face. The host pipe's face is
    // toggled unconditionally (meta layout {+X,-X,+Y,-Y,+Z,-Z}; meta==0 means
    // "all connected" (0x3F); opposite face = dir ^ 1). The neighbor's opposite
    // face is flipped only when the neighbor is itself a pipe/cable, i.e. the
    // mutual connection between two adjacent pipes/cables — so a standalone
    // pipe is still wrenchable without an adjacent pipe.
    if (r.error == "no_machine_at_position" && blockRepository_) {
        const int32_t x = p->x(), y = p->y(), z = p->z();
        const uint8_t face = action->face();
        if (face > 5) return;  // guard against malformed wire face
        blockRepository_->getBlock(x, y, z, [this, x, y, z, face](const BlockData& bd) {
            if (!(ItemId::isPipe(bd.block_id) || ItemId::isCable(bd.block_id))) {
                flatbuffers::FlatBufferBuilder fbb(128);
                auto err = fbb.CreateString("not_a_machine");
                auto resp = Protocol::CreateToolActionResp(fbb, false, err, 0, 0, 0, 0, 0);
                fbb.Finish(resp);
                std::vector<uint8_t> respData(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
                router_->Publish("player.tool.action.response", std::move(respData));
                return;
            }
            const int nx = x + kWrenchFaceDX[face], ny = y + kWrenchFaceDY[face], nz = z + kWrenchFaceDZ[face];
            blockRepository_->getBlock(nx, ny, nz, [this, x, y, z, face, bd, nx, ny, nz](const BlockData& nbd) {
                const bool nbIsPC = (nbd.block_id != 0) &&
                                    (ItemId::isPipe(nbd.block_id) || ItemId::isCable(nbd.block_id));
                // Toggle the host pipe's face unconditionally so a standalone
                // pipe is wrenchable; only flip the neighbor's opposite face
                // when the neighbor is itself a pipe/cable (mutual connection).
                auto toggle = computePipeToggle(face, bd.meta, nbIsPC ? nbd.meta : 0);
                const uint8_t newMHb = toggle.hostMeta;
                blockRepository_->setBlockCAS(x, y, z, bd.block_id, bd.block_id, newMHb,
                    [this, x, y, z, bid = bd.block_id, newMHb](const CASResult& cr) {
                        if (cr.status == 0) publishBlockChanged(x, y, z, bid, newMHb);
                    });
                if (nbIsPC) {
                    const uint8_t newMNb = toggle.neighborMeta;
                    blockRepository_->setBlockCAS(nx, ny, nz, nbd.block_id, nbd.block_id, newMNb,
                        [this, nx, ny, nz, nbid = nbd.block_id, newMNb](const CASResult& cr) {
                            if (cr.status == 0) publishBlockChanged(nx, ny, nz, nbid, newMNb);
                        });
                }
                flatbuffers::FlatBufferBuilder fbb(128);
                auto msg = fbb.CreateString(nbIsPC ? "Toggled pipe/cable connection."
                                                   : "Toggled pipe/cable face.");
                auto resp = Protocol::CreateToolActionResp(fbb, true, 0, 0, 0, 0, 0, msg);
                fbb.Finish(resp);
                std::vector<uint8_t> respData(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
                router_->Publish("player.tool.action.response", std::move(respData));
            });
        });
        return;
    }

    flatbuffers::FlatBufferBuilder fbb(128);
    auto err = r.error.empty() ? 0 : fbb.CreateString(r.error);
    auto roles = fbb.CreateVector(r.allRoles, 6);
    auto resp = Protocol::CreateToolActionResp(fbb, r.success, err, 0, 0, r.newRole, roles, 0);
    fbb.Finish(resp);
    std::vector<uint8_t> respData(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());

    router_->Publish("player.tool.action.response", std::move(respData));
}
} // namespace simcore