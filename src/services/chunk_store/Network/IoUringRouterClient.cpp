#include "IoUringRouterClient.h"

#include "core_generated.h"
#include "../Storage/ChunkStore.h"
#include <common/coords/Coords.h>
#include <spdlog/spdlog.h>

IoUringRouterClient::IoUringRouterClient(ChunkStore &store)
    : store_(store) {
    router_.on_publish = [this](const std::string &topic,
                                std::shared_ptr<std::vector<uint8_t>> data) {
        onPublish(topic, std::move(data));
    };
}

IoUringRouterClient::~IoUringRouterClient() {
    disconnect();
}

bool IoUringRouterClient::connect(const std::string &host, uint16_t port) {
    bool ok = router_.connect(host.c_str(), port, "chunkstore");
    if (ok) {
        spdlog::info("IoUringRouterClient: connected to {}:{} as chunkstore",
                     host, port);
        router_.subscribe("chunk.requests");
    } else {
        spdlog::error("IoUringRouterClient: failed to connect to {}:{}",
                      host, port);
    }
    return ok;
}

void IoUringRouterClient::disconnect() {
    router_.disconnect();
}

void IoUringRouterClient::onPublish(
    const std::string &topic,
    std::shared_ptr<std::vector<uint8_t>> data)
{
    if (topic != "chunk.requests" || !data || data->size() < 4)
        return;

    // "chunk.requests" publishes PlayerAction (CHUNK_REQUEST) FlatBuffers.
    auto *action = flatbuffers::GetRoot<Protocol::PlayerAction>(data->data());
    if (!action || !action->pos())
        return;
    if (action->action() != Protocol::PlayerActionType_CHUNK_REQUEST)
        return;

    auto *pos = action->pos();
    ChunkCoord coord{pos->x(), pos->y(), pos->z()};

    store_.AsyncGetChunk(coord,
        [this, coord](std::shared_ptr<std::vector<uint8_t>> palette) {
            if (!palette || palette->empty())
                return;
            publishChunkLoadedCompressed(
                coord.x, coord.y, coord.z, std::move(palette));
        });
}

void IoUringRouterClient::publishChunkLoadedCompressed(
    int32_t cx, int32_t cy, int32_t cz,
    std::shared_ptr<std::vector<uint8_t>> palette)
{
    flatbuffers::FlatBufferBuilder fb(palette->size() + 128);
    Protocol::Vec3i coord(cx, cy, cz);
    auto palette_fb = fb.CreateVector(palette->data(), palette->size());
    auto compressed = Protocol::CreateCompressedChunkData(fb, &coord, palette_fb);
    fb.Finish(compressed);

    router_.publish("world.chunk.loaded.compressed",
                    fb.GetBufferPointer(), fb.GetSize());
}
