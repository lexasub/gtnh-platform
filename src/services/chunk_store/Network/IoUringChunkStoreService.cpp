#include "IoUringChunkStoreService.h"

#include "chunkstore_generated.h"
#include "FrameCodec.h"
#include "../Storage/ChunkStore.h"
#include "../Storage/cache/MutableChunk.h"
#include <common/coords/Coords.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

IoUringChunkStoreService::IoUringChunkStoreService(ChunkStore &store)
    : store_(store) {
    server_.on_accept = [this](int client_fd) { onAccept(client_fd); };
}

IoUringChunkStoreService::~IoUringChunkStoreService() {
    stop();
}

bool IoUringChunkStoreService::listen(uint16_t port) {
    running_ = true;
    bool ok = server_.listen(port, "chunkd");
    if (ok) {
        spdlog::info("IoUringChunkStoreService listening on port {}", port);
    } else {
        spdlog::error("IoUringChunkStoreService: failed to listen on port {}",
                      port);
    }
    return ok;
}

void IoUringChunkStoreService::stop() {
    if (!running_.exchange(false))
        return;
    server_.stop();
    std::lock_guard lock(connsMutex_);
    for (auto &conn : connections_) {
        if (conn->is_open())
            conn->close();
    }
    connections_.clear();
    spdlog::info("IoUringChunkStoreService stopped");
}

void IoUringChunkStoreService::onAccept(int client_fd) {
    auto tags = tagAlloc_.alloc();
    auto conn = std::make_shared<gtnh::net::IoUringConnection>(
        client_fd, "chunkd-client", tags);

    conn->on_message = [this, conn](uint8_t type, const uint8_t *data,
                                     size_t len) {
        onMessage(conn, type, data, len);
    };
    conn->on_closed = [this, conn]() { onClosed(conn); };

    {
        std::lock_guard lock(connsMutex_);
        connections_.push_back(conn);
    }

    conn->start_reading();
}

void IoUringChunkStoreService::onMessage(
    std::shared_ptr<gtnh::net::IoUringConnection> conn,
    uint8_t msg_type, const uint8_t *data, size_t len)
{
    (void)msg_type;

    // Parse FlatBuffer frame — same protocol as old ChunkStoreService
    flatbuffers::Verifier verifier(data, len);
    if (!Protocol::VerifyChunkStoreFrameBuffer(verifier)) {
        spdlog::error("IoUringChunkStoreService: invalid FlatBuffer");
        return;
    }
    auto *frame = flatbuffers::GetRoot<Protocol::ChunkStoreFrame>(data);
    if (!frame || !frame->payload())
        return;
    auto *msg = frame->payload_as_ChunkStoreMessage();
    if (!msg)
        return;

    switch (msg->request_type()) {
    case Protocol::ChunkStoreRequest_GetBlockReq:
        handleGetBlock(conn, msg->req_id(), msg);
        break;
    case Protocol::ChunkStoreRequest_SetBlockReq:
        handleSetBlock(conn, msg->req_id(), msg);
        break;
    case Protocol::ChunkStoreRequest_SaveChunkReq:
        handleSaveChunk(conn, msg->req_id(), msg);
        break;
    case Protocol::ChunkStoreRequest_SetBlockCASReq:
        handleSetBlockCAS(conn, msg->req_id(), msg);
        break;
    default:
        spdlog::warn("IoUringChunkStoreService: unknown request type");
    }
}

void IoUringChunkStoreService::onClosed(
    std::shared_ptr<gtnh::net::IoUringConnection> conn)
{
    std::lock_guard lock(connsMutex_);
    auto it = std::find(connections_.begin(), connections_.end(), conn);
    if (it != connections_.end())
        connections_.erase(it);
}

void IoUringChunkStoreService::handleGetBlock(
    std::shared_ptr<gtnh::net::IoUringConnection> conn,
    uint32_t req_id, const Protocol::ChunkStoreMessage *req)
{
    auto *pos = req->request_as_GetBlockReq()->pos();
    BlockPos bp{pos->x(), pos->y(), pos->z()};
    uint16_t block = store_.GetBlockAt(bp);
    uint8_t meta = store_.GetMeta(bp.x, bp.y, bp.z);
    uint32_t mb_id = store_.GetMultiblock(bp.x, bp.y, bp.z);

    flatbuffers::FlatBufferBuilder fb(64);
    auto resp = Protocol::CreateGetBlockResp(fb, block, meta, mb_id);
    auto reply = Protocol::CreateChunkStoreReply(
        fb, req_id, Protocol::ChunkStoreResponse_GetBlockResp, resp.Union());
    auto frame = Protocol::CreateChunkStoreFrame(
        fb, Protocol::ChunkStorePayload_ChunkStoreReply, reply.Union());
    fb.Finish(frame);
    sendResponse(conn, fb);
}

void IoUringChunkStoreService::handleSetBlock(
    std::shared_ptr<gtnh::net::IoUringConnection> conn,
    uint32_t req_id, const Protocol::ChunkStoreMessage *req)
{
    auto *pos = req->request_as_SetBlockReq()->pos();
    BlockPos bp{pos->x(), pos->y(), pos->z()};
    uint16_t block_id = req->request_as_SetBlockReq()->block_id();
    uint8_t meta = req->request_as_SetBlockReq()->meta();

    ChunkCoord coord{bp.x >> 5, bp.y >> 5, bp.z >> 5};
    BlockPos local{bp.x & 31, bp.y & 31, bp.z & 31};

    // AsyncSetBlock calls back on completion — send response then
    store_.AsyncSetBlock(coord, local, block_id, meta, 0,
        [this, conn, req_id](bool success) {
            flatbuffers::FlatBufferBuilder fb(64);
            auto resp = Protocol::CreateSetBlockResp(fb, success);
            auto reply = Protocol::CreateChunkStoreReply(
                fb, req_id, Protocol::ChunkStoreResponse_SetBlockResp,
                resp.Union());
            auto frame = Protocol::CreateChunkStoreFrame(
                fb, Protocol::ChunkStorePayload_ChunkStoreReply,
                reply.Union());
            fb.Finish(frame);
            sendResponse(conn, fb);
        });
}

void IoUringChunkStoreService::handleSaveChunk(
    std::shared_ptr<gtnh::net::IoUringConnection> conn,
    uint32_t req_id, const Protocol::ChunkStoreMessage *req)
{
    auto *coord = req->request_as_SaveChunkReq()->coord();
    ChunkCoord c{coord->x(), coord->y(), coord->z()};
    auto *blocks_fb = req->request_as_SaveChunkReq()->blocks();
    uint16_t blocks[SEC_VOL * SEC_CNT] = {};
    if (blocks_fb &&
        blocks_fb->size() == SEC_VOL * SEC_CNT * sizeof(uint16_t))
        std::memcpy(blocks, blocks_fb->data(),
                    SEC_VOL * SEC_CNT * sizeof(uint16_t));

    auto chunk = std::make_shared<MutableChunk>(
        MutableChunk::fromBlocks(blocks));
    store_.AsyncSaveChunk(std::move(chunk), c,
        [this, conn, req_id](bool success) {
            flatbuffers::FlatBufferBuilder fb(64);
            auto resp = Protocol::CreateSaveChunkResp(fb, success);
            auto reply = Protocol::CreateChunkStoreReply(
                fb, req_id, Protocol::ChunkStoreResponse_SaveChunkResp,
                resp.Union());
            auto frame = Protocol::CreateChunkStoreFrame(
                fb, Protocol::ChunkStorePayload_ChunkStoreReply,
                reply.Union());
            fb.Finish(frame);
            sendResponse(conn, fb);
        });
}

void IoUringChunkStoreService::handleSetBlockCAS(
    std::shared_ptr<gtnh::net::IoUringConnection> conn,
    uint32_t req_id, const Protocol::ChunkStoreMessage *req)
{
    auto *cas_req = req->request_as_SetBlockCASReq();
    if (!cas_req || !cas_req->pos())
        return;

    auto *pos = cas_req->pos();
    auto result = store_.casBlock(
        pos->x(), pos->y(), pos->z(),
        cas_req->expected_block_id(),
        cas_req->new_block_id(),
        cas_req->meta());

    Protocol::CASStatus status =
        (result.status == ChunkStore::CASResult::Ok)
            ? Protocol::CASStatus::CASStatus_OK
            : Protocol::CASStatus::CASStatus_CONFLICT;

    flatbuffers::FlatBufferBuilder fb(64);
    auto resp = Protocol::CreateSetBlockCASResp(fb, status,
        result.actual_block, result.actual_meta);
    auto reply = Protocol::CreateChunkStoreReply(
        fb, req_id, Protocol::ChunkStoreResponse_SetBlockCASResp,
        resp.Union());
    auto frame = Protocol::CreateChunkStoreFrame(
        fb, Protocol::ChunkStorePayload_ChunkStoreReply, reply.Union());
    fb.Finish(frame);
    sendResponse(conn, fb);
}

void IoUringChunkStoreService::sendResponse(
    std::shared_ptr<gtnh::net::IoUringConnection> conn,
    const flatbuffers::FlatBufferBuilder &fb)
{
    // IoUringConnection::send() prepends [4B length][1B type] automatically.
    // Type 0 matches the dummy byte the old service used.
    if (conn->is_open())
        conn->send(0, fb.GetBufferPointer(), fb.GetSize());
}
