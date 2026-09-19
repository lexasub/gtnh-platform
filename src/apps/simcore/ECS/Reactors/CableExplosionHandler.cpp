#include "CableExplosionHandler.h"
#include "pipe_network_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

CableExplosionHandler::CableExplosionHandler(std::shared_ptr<IoUringChunkClient> chunkClient)
    : chunkClient_(std::move(chunkClient))
{}

void CableExplosionHandler::handle(const std::vector<uint8_t>& data) {
    auto* event = flatbuffers::GetRoot<Protocol::CableExplodedEvent>(data.data());
    if (!event || !event->pos()) return;

    int32_t x = event->pos()->x();
    int32_t y = event->pos()->y();
    int32_t z = event->pos()->z();
    uint64_t node_id = event->node_id();
    float temperature = event->temperature();

    if (chunkClient_) {
        chunkClient_->SetBlock(x, y, z, 0, 0,
            [node_id, x, y, z, temperature](bool success) {
                if (success) {
                    spdlog::info("[CableExplosion] cable node {} at ({},{},{}) replaced with air (temp={:.1f})",
                                 node_id, x, y, z, temperature);
                } else {
                    spdlog::warn("[CableExplosion] SetBlock failed for cable node {} at ({},{},{})",
                                 node_id, x, y, z);
                }
            });
    } else {
        spdlog::error("[CableExplosion] No chunk client available for SetBlock at ({},{},{})", x, y, z);
    }
}

} // namespace simcore
