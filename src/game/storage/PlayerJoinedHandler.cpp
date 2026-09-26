#include "PlayerJoinedHandler.h"
#include "PlayerInventoryStore.h"
#include <game/quests/QuestManager.h>
#include <apps/simcore/Network/clients/IoUringRouterClient.h>
#include "core_generated.h"
#include "quest_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
PlayerJoinedHandler::PlayerJoinedHandler(std::shared_ptr<PlayerInventoryStore> inv,
                                         std::shared_ptr<IoUringRouterClient> router,
                                         std::shared_ptr<QuestManager> questManager)
    : inventoryStore_(std::move(inv)), router_(std::move(router)),
      questManager_(std::move(questManager)) {}
void PlayerJoinedHandler::handle(const std::vector<uint8_t>& data) {
    // gp-ajvg: see InventoryLoadHandler::handle for the full account. "player.
    // joined" is likewise a router-subscribed topic (main.cpp / subscribeAll),
    // so any publisher can deliver a zero-length or truncated payload; the
    // Verifier-less GetRoot() below handed the join bootstrap a Table whose
    // first accessor dereferenced nullptr for an empty vector. Verify BEFORE
    // the buffer pointer is read — same R1/R2 rejection as
    // InventoryActionHandler.cpp:31-34.
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::PlayerJoined>(nullptr)) {
        spdlog::warn("[SimCore] PlayerJoined: invalid PlayerJoined buffer ({} bytes) — dropped",
                     data.size());
        return;
    }
    auto joined = flatbuffers::GetRoot<Protocol::PlayerJoined>(data.data());
    if (!joined) return;
    uint64_t pid = joined->player_id();
    spdlog::info("[SimCore] Player joined: id={}", pid);
    inventoryStore_->initPlayer(pid);
    if (questManager_) {
        questManager_->onPlayerJoined(pid);
    }
    if (router_) {
        // Request quest progress restore via FlatBuffers QuestProgressUpdate
        // (empty quests vector = query). Matches MetaDB HandleQuestGet.
        flatbuffers::FlatBufferBuilder builder(32);
        auto req = Protocol::CreateQuestProgressUpdate(builder, pid);
        builder.Finish(req);
        router_->PublishRaw("meta_db.quest.get", builder.GetBufferPointer(), builder.GetSize());
        spdlog::info("[SimCore] Requested quest progress restore for player {}", pid);
    }
}
} // namespace simcore
