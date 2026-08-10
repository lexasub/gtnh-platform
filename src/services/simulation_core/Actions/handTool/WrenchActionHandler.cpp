#include "WrenchActionHandler.h"
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

    // cycleFace already did the ECS machine lookup. For a pipe target, defer the
    // response: PipeWrenchResponseHandler answers on `pipe.wrench.response`.
    if (r.error == "no_machine_at_position" && blockRepository_) {
        const uint64_t pid = action->player_id();
        const int32_t x = p->x(), y = p->y(), z = p->z();
        const uint8_t face = action->face();
        blockRepository_->getBlock(x, y, z, [this, pid, x, y, z, face](const BlockData& bd) {
            if (ItemId::isPipe(bd.block_id)) {
                flatbuffers::FlatBufferBuilder fbb(64);
                Protocol::Vec3i pos(x, y, z);
                auto a = Protocol::CreatePipeWrenchAction(fbb, pid, &pos, face);
                fbb.Finish(a);
                std::vector<uint8_t> buf(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
                router_->Publish("pipe.wrench.action", std::move(buf));
                return;
            }
            flatbuffers::FlatBufferBuilder fbb(128);
            auto err = fbb.CreateString("not_a_machine");
            auto resp = Protocol::CreateToolActionResp(fbb, false, err, 0, 0, 0, 0, 0);
            fbb.Finish(resp);
            std::vector<uint8_t> respData(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
            router_->Publish("player.tool.action.response", std::move(respData));
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