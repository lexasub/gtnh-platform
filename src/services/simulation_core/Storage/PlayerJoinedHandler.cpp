#include "PlayerJoinedHandler.h"
#include "PlayerInventoryStore.h"
#include "Quest/QuestManager.h"
#include "Network/clients/IoUringRouterClient.h"
#include "core_generated.h"
#include <cstring>
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
        uint8_t buf[8];
        std::memcpy(buf, &pid, sizeof(pid));
        router_->PublishRaw("meta_db.quest.get", buf, sizeof(buf));
        spdlog::info("[SimCore] Requested quest progress restore for player {}", pid);
    }
}
} // namespace simcore
