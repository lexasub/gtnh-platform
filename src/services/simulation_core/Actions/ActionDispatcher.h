#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include <functional>
#include <flatbuffers/verifier.h>

namespace simcore {

    class SetBlockCASHandler;

    using ItemGiveCallback = std::function<void(uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot)>;

    class ActionDispatcher {
    public:
        ActionDispatcher(std::shared_ptr<SetBlockCASHandler> casHandler,
                         ItemGiveCallback onGiveItem = nullptr);

        void dispatch(const std::vector<uint8_t>& data);

    private:
        bool tryParseAsPlayerAction(const std::vector<uint8_t> &data, flatbuffers::Verifier &verifier);
        std::shared_ptr<SetBlockCASHandler> casHandler_;
        ItemGiveCallback onGiveItem_;
    };

} // namespace simcore