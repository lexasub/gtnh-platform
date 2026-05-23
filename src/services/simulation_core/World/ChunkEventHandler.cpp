#include "World/ChunkEventHandler.h"
#include "ECS/SimulationEngine.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

    ChunkEventHandler::ChunkEventHandler(std::shared_ptr<SimulationEngine> engine)
        : engine_(std::move(engine))
    {}

    void ChunkEventHandler::handle(const std::vector<uint8_t>& flatbuffer_data)
    {
        if (flatbuffer_data.empty()) {
            spdlog::warn("ChunkEventHandler: empty data received");
            return;
        }

        // world.blocks.changed carries a serialized BlockChangedEvent (single block change, ~28 bytes)
        flatbuffers::Verifier verifier(flatbuffer_data.data(), flatbuffer_data.size());
        if (!verifier.VerifyBuffer<Protocol::BlockChangedEvent>()) {
            spdlog::warn("ChunkEventHandler: invalid BlockChangedEvent FlatBuffer");
            return;
        }

        const auto* event = flatbuffers::GetRoot<Protocol::BlockChangedEvent>(flatbuffer_data.data());
        auto pos = event->pos();
        if (!pos) {
            spdlog::warn("ChunkEventHandler: BlockChangedEvent missing position");
            return;
        }

        int32_t x = pos->x();
        int32_t y = pos->y();
        int32_t z = pos->z();
        uint16_t block_id = event->block_id();
        uint8_t meta = event->meta();
        uint32_t mb_id = event->mb_id();

        spdlog::debug("Block changed at ({},{},{}) id={} meta={} mb_id={}",
                      x, y, z, block_id, meta, mb_id);

        engine_->onBlockChanged(static_cast<uint32_t>(x),
                                static_cast<uint32_t>(y),
                                static_cast<uint32_t>(z),
                                block_id, meta, mb_id);
    }

} // namespace simcore