#include "PlayerJoinedHandler.h"
#include "PlayerInventoryStore.h"
#include "core_generated.h"
#include <spdlog/spdlog.h>
namespace simcore {
PlayerJoinedHandler::PlayerJoinedHandler(std::shared_ptr<PlayerInventoryStore> inv) : inventoryStore_(std::move(inv)) {}
void PlayerJoinedHandler::handle(const std::vector<uint8_t>& data) {
    auto joined = flatbuffers::GetRoot<Protocol::PlayerJoined>(data.data());
    uint64_t pid = joined->player_id();
    spdlog::info("[SimCore] Player joined: id={}", pid);
    inventoryStore_->initPlayer(pid);
}
} // namespace simcore
