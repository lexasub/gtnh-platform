#include "Actions/ActionDispatcher.h"
#include "Actions/SetBlockCASHandler.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

    ActionDispatcher::ActionDispatcher(std::shared_ptr<SetBlockCASHandler> casHandler,
                                       ItemGiveCallback onGiveItem)
        : casHandler_(std::move(casHandler))
        , onGiveItem_(std::move(onGiveItem))
    {}

    void ActionDispatcher::dispatch(const std::vector<uint8_t>& data, const std::string& topic)
    {
        spdlog::info("ActionDispatcher: dispatch topic='{}' size={}", topic, data.size());

        if (topic == "player.setblock") {
            flatbuffers::Verifier v(data.data(), data.size());
            if (!v.VerifyBuffer<Protocol::SetBlockAction>()) {
                spdlog::error("ActionDispatcher: invalid SetBlockAction FlatBuffer");
                return;
            }
            const Protocol::SetBlockAction* action = flatbuffers::GetRoot<Protocol::SetBlockAction>(data.data());
            auto action_type = action->action();
            if (action_type != Protocol::PlayerActionType_LEFT_MOUSE_CLICK && action_type != Protocol::PlayerActionType_RIGHT_MOUSE_CLICK) {
                spdlog::warn("ActionDispatcher: unhandled SetBlockAction action type {}",
                             static_cast<int>(action_type));
                return;
            }
            if (casHandler_) {
                casHandler_->handle((void*)action);
            } else {
                spdlog::warn("ActionDispatcher: CAS handler not available, dropping SetBlockAction");
            }
            return;
        }

        flatbuffers::Verifier v(data.data(), data.size());
        if (!v.VerifyBuffer<Protocol::PlayerAction>()) {
            spdlog::error("ActionDispatcher: invalid PlayerAction FlatBuffer on topic '{}'", topic);
            return;
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
            return;
        }
        case Protocol::PlayerActionType_CHUNK_REQUEST: {
            auto* pos = pa->pos();
            if (pos) {
                spdlog::info("CHUNK_REQUEST: player={} chunk=({},{},{})",
                             pa->player_id(), pos->x(), pos->y(), pos->z());
            }
            return;
        }
        case Protocol::PlayerActionType_UNLOAD:
        case Protocol::PlayerActionType_MOVE:
            return;
        default:
            spdlog::warn("ActionDispatcher: unhandled PlayerAction type {}",
                         static_cast<int>(pa->action()));
            return;
        }
    }

} // namespace simcore