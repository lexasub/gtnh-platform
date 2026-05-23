#pragma once
#include <memory>
#include <vector>
#include <functional>
#include "IActionHandler.h"

#include "core_generated.h"

namespace simcore {

    class IBlockRepository;
    class IEventPublisher;
    class SimulationEngine;

    using ItemGiveCallback = std::function<void(uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot)>;
    using DrillUseCallback = std::function<void(uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id)>;

    class SetBlockCASHandler : public IActionHandler {
    public:
        SetBlockCASHandler(std::shared_ptr<IBlockRepository> repo,
                           std::shared_ptr<IEventPublisher> publisher,
                           std::shared_ptr<SimulationEngine> engine,
                           ItemGiveCallback onGiveItem = nullptr,
                           DrillUseCallback onDrillUse = nullptr);

        void handle(const void *table) override;

    private:
        auto handle(const Protocol::SetBlockAction *action) -> void;
        std::shared_ptr<IBlockRepository> repo_;
        std::shared_ptr<IEventPublisher> publisher_;
        std::shared_ptr<SimulationEngine> engine_;
        ItemGiveCallback onGiveItem_;
        DrillUseCallback onDrillUse_;
    };

} // namespace simcore