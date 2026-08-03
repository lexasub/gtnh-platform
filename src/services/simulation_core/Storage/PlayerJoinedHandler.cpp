#include "PlayerJoinedHandler.h"
#include "PlayerInventoryStore.h"
#include "Quest/QuestManager.h"
#include "Network/clients/IoUringRouterClient.h"
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
    auto joined = flatbuffers::GetRoot<Protocol::PlayerJoined>(data.data());
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
