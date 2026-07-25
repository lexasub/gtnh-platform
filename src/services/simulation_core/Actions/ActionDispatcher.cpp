#include "Actions/ActionDispatcher.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

    ActionDispatcher::ActionDispatcher(ItemGiveCallback onGiveItem)
        : onGiveItem_(std::move(onGiveItem))
    {}

    bool ActionDispatcher::tryParseAsPlayerAction(const std::vector<uint8_t> &data, flatbuffers::Verifier &verifier) {
        if (!verifier.VerifyBuffer<Protocol::PlayerAction>()) {
            return false;
        }
        auto* pa = flatbuffers::GetRoot<Protocol::PlayerAction>(data.data());

        switch (pa->action()) {
        case Protocol::PlayerActionType_ITEM_ACTION: {
            uint16_t item_id = pa->block_id();
            uint8_t count = pa->count();
            if (onGiveItem_) {
                onGiveItem_(pa->player_id(), item_id, count, pa->pos()->x());
            }
            spdlog::info("ITEM_ACTION: player={} item_id={} count={} target_slot={}",
                         pa->player_id(), item_id, count, pa->pos()->x());
            return true;
        }
        case Protocol::PlayerActionType_CHUNK_REQUEST: {
            auto* pos = pa->pos();
            if (pos) {
                spdlog::info("CHUNK_REQUEST: player={} chunk=({},{},{})",
                             pa->player_id(), pos->x(), pos->y(), pos->z());
            }
            return true;
        }
        default:
            return false;
        }
    }

    void ActionDispatcher::dispatch(const std::vector<uint8_t>& data)
    {
        flatbuffers::Verifier v(data.data(), data.size());
        tryParseAsPlayerAction(data, v);
    }

} // namespace simcore