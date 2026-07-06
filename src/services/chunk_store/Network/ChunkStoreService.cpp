#include "ChunkStoreService.h"
#include "FrameCodec.h"
#include <spdlog/spdlog.h>
#include <utility>
#include "../World/ServerWorld.h"
#include "../Storage/ChunkStore.h"
#include <common/coords/Coords.h>

ChunkStoreService::ChunkStoreService(ServerWorld& world, uint16_t port)
    : world_(world), io_context_(),
      write_strand_(io_context_.get_executor()),
      acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {}

ChunkStoreService::~ChunkStoreService() { stop(); }

void ChunkStoreService::start() {
    running_ = true;
    doAccept();
    unsigned int n = std::thread::hardware_concurrency();
    if (n == 0) n = 2;
    for (unsigned i = 0; i < n; ++i) {
        workers_.emplace_back([this] { io_context_.run(); });
    }
    spdlog::info("ChunkStoreService listening on port {}", acceptor_.local_endpoint().port());
}

void ChunkStoreService::stop() {
    if (!running_.exchange(false)) return;
    io_context_.stop();
    acceptor_.close();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    spdlog::info("ChunkStoreService stopped");
}

void ChunkStoreService::doAccept() {
    acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (!ec && running_) {
            auto client = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
            spdlog::debug("Client connected: {}", client->remote_endpoint().address().to_string());
            std::shared_ptr<asio::ip::tcp::socket> socket = client;
            doReadSize(socket);
        }
        if (running_) doAccept();
    });
}

void ChunkStoreService::doReadSize(std::shared_ptr<asio::ip::tcp::socket> socket) {
    auto size_buf = std::make_shared<std::array<uint8_t, 4>>();
    asio::async_read(*socket, asio::buffer(*size_buf),
        [this, socket, size_buf](std::error_code ec, size_t) {
            if (ec) {
                if (ec != asio::error::operation_aborted)
                    spdlog::debug("Client disconnected (size read)");
                return;
            }
            uint32_t msg_size = FrameCodec::readBE32(size_buf->data());
            if (msg_size < 4 || msg_size > 2*1024*1024) {
                spdlog::error("Invalid msg size: {}", msg_size);
                return;
            }
            doReadPayload(socket, msg_size);
        });
}

void ChunkStoreService::onReadPayload(std::error_code ec, std::shared_ptr<asio::ip::tcp::socket> socket,
                                      std::shared_ptr<std::vector<unsigned char> > payload) {
    if (ec) return;
    auto verifier = flatbuffers::Verifier(payload->data(), payload->size());
    if (!Protocol::VerifyChunkStoreFrameBuffer(verifier)) {
        spdlog::error("Invalid FlatBuffer");
        return;
    }
    auto* frame = flatbuffers::GetRoot<Protocol::ChunkStoreFrame>(payload->data());
    if (!frame || !frame->payload()) return;
    auto* msg = frame->payload_as_ChunkStoreMessage();
    if (!msg) return;

    // dispatch
    switch (msg->request_type()) {
        case Protocol::ChunkStoreRequest_GetBlockReq:
            handleGetBlock(socket, msg->req_id(), msg);
            break;
        case Protocol::ChunkStoreRequest_SetBlockReq:
            handleSetBlock(socket, msg->req_id(), msg);
            break;
        case Protocol::ChunkStoreRequest_SaveChunkReq:
            handleSaveChunk(socket, msg->req_id(), msg);
            break;
        case Protocol::ChunkStoreRequest_SetBlockCASReq:
            handleSetBlockCAS(socket, msg->req_id(), msg);
            break;
        default:
            spdlog::warn("Unknown request type");
    }
    doReadSize(socket); // continue reading
}

void ChunkStoreService::doReadPayload(std::shared_ptr<asio::ip::tcp::socket> socket, uint32_t msg_size) {
    auto payload = std::make_shared<std::vector<uint8_t>>(msg_size);
    asio::async_read(*socket, asio::buffer(*payload),
        [this, socket, payload](std::error_code ec, size_t) {
            onReadPayload(ec, socket, payload);
        });
}

// ---------------------------------------------------------------------------
// Request handlers (async)
// ---------------------------------------------------------------------------
void ChunkStoreService::handleGetBlock(std::shared_ptr<asio::ip::tcp::socket> socket,
                                        uint32_t req_id,
                                        const Protocol::ChunkStoreMessage* req) {
    auto* pos = req->request_as_GetBlockReq()->pos();
    BlockPos bp{pos->x(), pos->y(), pos->z()};
    // GetBlockAt – синхронный, но быстрый (кэш+LMDB read-only). Можно оставить.
    uint16_t block = world_.GetBlockAt(bp);
    uint8_t  meta  = world_.GetChunkStore()->GetMeta(bp.x, bp.y, bp.z);
    uint32_t mb_id = world_.GetChunkStore()->GetMultiblock(bp.x, bp.y, bp.z);
    // Отправляем ответ
    flatbuffers::FlatBufferBuilder fb(64);
    auto resp = Protocol::CreateGetBlockResp(fb, block, meta, mb_id);
    auto reply = Protocol::CreateChunkStoreReply(fb, req_id,
            Protocol::ChunkStoreResponse_GetBlockResp, resp.Union());
    auto frame = Protocol::CreateChunkStoreFrame(fb, Protocol::ChunkStorePayload_ChunkStoreReply, reply.Union());
    fb.Finish(frame);
    EnqueueWrite(socket, fb);
}

void ChunkStoreService::handleSetBlock(std::shared_ptr<asio::ip::tcp::socket> socket,
                                        uint32_t req_id,
                                        const Protocol::ChunkStoreMessage* req) {
    auto* pos = req->request_as_SetBlockReq()->pos();
    BlockPos bp{pos->x(), pos->y(), pos->z()};
    uint16_t block_id = req->request_as_SetBlockReq()->block_id();
    uint8_t  meta     = req->request_as_SetBlockReq()->meta();

    // Асинхронная установка блока
    world_.SetBlockAsync(bp, block_id, meta, 0, [this, socket, req_id](bool success) {
        flatbuffers::FlatBufferBuilder fb(64);
        auto resp = Protocol::CreateSetBlockResp(fb, success);
        auto reply = Protocol::CreateChunkStoreReply(fb, req_id,
                     Protocol::ChunkStoreResponse_SetBlockResp, resp.Union());
        auto frame = Protocol::CreateChunkStoreFrame(fb, Protocol::ChunkStorePayload_ChunkStoreReply, reply.Union());
        fb.Finish(frame);
        // Отправляем ответ в том же io_context (потокобезопасно)
        asio::post(io_context_, [this, socket, fb = std::move(fb)]() mutable {
            EnqueueWrite(socket, fb);
        });
    });
}

void ChunkStoreService::handleSaveChunk(std::shared_ptr<asio::ip::tcp::socket> socket,
                                         uint32_t req_id,
                                         const Protocol::ChunkStoreMessage* req) {
    auto* coord = req->request_as_SaveChunkReq()->coord();
    ChunkCoord c{coord->x(), coord->y(), coord->z()};
    // Восстанавливаем Chunk из FlatBuffer
    auto chunk = std::make_shared<Chunk>();
    auto* blocks_fb = req->request_as_SaveChunkReq()->blocks();
    auto* meta_fb   = req->request_as_SaveChunkReq()->meta();
    auto* mb_fb     = req->request_as_SaveChunkReq()->multiblock();
    if (blocks_fb && blocks_fb->size() == Chunk::VOLUME)
        std::memcpy(chunk->blocks.data(), blocks_fb->data(), Chunk::VOLUME*sizeof(uint16_t));
    if (meta_fb && meta_fb->size() == Chunk::VOLUME)
        std::memcpy(chunk->meta.data(), meta_fb->data(), Chunk::VOLUME*sizeof(uint8_t));
    if (mb_fb && mb_fb->size() == Chunk::VOLUME)
        std::memcpy(chunk->multiblock.data(), mb_fb->data(), Chunk::VOLUME*sizeof(uint32_t));
    world_.GetChunkStore()->AsyncSaveChunk(std::move(chunk), c,
        [this, socket, req_id](bool success) {
            flatbuffers::FlatBufferBuilder fb(64);
            auto resp = Protocol::CreateSaveChunkResp(fb, success);
            auto reply = Protocol::CreateChunkStoreReply(fb, req_id,
                         Protocol::ChunkStoreResponse_SaveChunkResp, resp.Union());
            auto frame = Protocol::CreateChunkStoreFrame(fb, Protocol::ChunkStorePayload_ChunkStoreReply, reply.Union());
            fb.Finish(frame);
            asio::post(io_context_, [this, socket, fb = std::move(fb)]() mutable {
                EnqueueWrite(socket, fb);
            });
        });
}

void ChunkStoreService::handleSetBlockCAS(std::shared_ptr<asio::ip::tcp::socket> socket,
                                          uint32_t req_id,
                                          const Protocol::ChunkStoreMessage* req) {
    auto* cas_req = req->request_as_SetBlockCASReq();
    if (!cas_req || !cas_req->pos()) {
        sendError(socket, req_id, "Invalid SetBlockCAS request");
        return;
    }

    auto* pos = cas_req->pos();
    BlockPos bp{pos->x(), pos->y(), pos->z()};
    uint16_t expected_id = cas_req->expected_block_id();
    uint16_t new_block_id = cas_req->new_block_id();
    uint8_t  meta         = cas_req->meta();

    ChunkCoord cc{bp.x >> 5, bp.y >> 5, bp.z >> 5};
    int32_t lx = bp.x & 31;
    int32_t ly = bp.y & 31;
    int32_t lz = bp.z & 31;
    uint32_t idx = (static_cast<uint32_t>(ly) << 10) |
                   (static_cast<uint32_t>(lz) <<  5) |
                   (static_cast<uint32_t>(lx));

    const Chunk* chunk = world_.GetChunk(cc);
    if (!chunk) {
        sendError(socket, req_id, "Chunk not loaded");
        return;
    }

    uint16_t current_block = chunk->blocks[idx];
    uint8_t  current_meta  = chunk->meta[idx];

    Protocol::CASStatus status;
    uint16_t actual_block_id;
    uint8_t  actual_meta;

    if (current_block == expected_id) {
        chunk->blocks[idx] = new_block_id;
        chunk->meta[idx]   = meta;
        status = Protocol::CASStatus::CASStatus_OK;
        actual_block_id = new_block_id;
        actual_meta     = meta;

        // Mark dirty for batch LMDB flush
        world_.GetChunkStore()->markDirty(cc.x, cc.y, cc.z);
    } else {
        status = Protocol::CASStatus::CASStatus_CONFLICT;
        actual_block_id = current_block;
        actual_meta     = current_meta;
    }

    flatbuffers::FlatBufferBuilder fb(64);
    auto resp = Protocol::CreateSetBlockCASResp(fb, status, actual_block_id, actual_meta);
    auto reply = Protocol::CreateChunkStoreReply(fb, req_id,
                 Protocol::ChunkStoreResponse_SetBlockCASResp, resp.Union());
    auto frame = Protocol::CreateChunkStoreFrame(fb, Protocol::ChunkStorePayload_ChunkStoreReply, reply.Union());
    fb.Finish(frame);
    EnqueueWrite(socket, fb);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void ChunkStoreService::sendResponse(std::shared_ptr<asio::ip::tcp::socket> socket,
                                     const flatbuffers::FlatBufferBuilder& fb) {
    EnqueueWrite(socket, fb);
}

void ChunkStoreService::sendError(std::shared_ptr<asio::ip::tcp::socket> socket,
                                   uint32_t req_id, const std::string& msg) {
    flatbuffers::FlatBufferBuilder fb(256);
    auto err_str = fb.CreateString(msg);
    auto resp = Protocol::CreateErrorResp(fb, err_str);
    auto reply = Protocol::CreateChunkStoreReply(fb, req_id,
                 Protocol::ChunkStoreResponse_ErrorResp, resp.Union());
    auto frame = Protocol::CreateChunkStoreFrame(fb, Protocol::ChunkStorePayload_ChunkStoreReply, reply.Union());
    fb.Finish(frame);
    EnqueueWrite(socket, fb);
}

void ChunkStoreService::EnqueueWrite(std::shared_ptr<asio::ip::tcp::socket> socket,
                                      const flatbuffers::FlatBufferBuilder& fb) {
    uint32_t size = fb.GetSize();
    // Include a 1-byte dummy message type to match IoUringClientConnection expectations
    uint32_t total_len = size + 1; // payload length = msg_type(1) + flatbuffer size
    uint32_t frame_size = 4 + total_len; // 4-byte length prefix + total_len bytes
    auto frame = std::make_shared<std::vector<uint8_t>>(frame_size);
    
    // Write big-endian length (total_len)
    (*frame)[0] = static_cast<uint8_t>((total_len) >> 24);
    (*frame)[1] = static_cast<uint8_t>((total_len) >> 16);
    (*frame)[2] = static_cast<uint8_t>((total_len) >> 8);
    (*frame)[3] = static_cast<uint8_t>(total_len);
    
    // Dummy message type (can be 0)
    (*frame)[4] = 0;
    
    // Copy flatbuffer payload after the msg_type byte
    std::memcpy(frame->data() + 5, fb.GetBufferPointer(), size);
    
    asio::post(write_strand_, [this, socket, frame = std::move(frame)]() mutable {
        bool was_empty = write_queue_.empty();
        write_queue_.push_back(WriteOp{std::move(frame), socket});
        if (was_empty) {
            DoWrite();
        }
    });
}

void ChunkStoreService::DoWrite() {
    if (write_queue_.empty()) return;
    
    auto op = std::move(write_queue_.front());
    write_queue_.pop_front();
    auto buf = asio::buffer(*op.frame);
    auto socket = std::move(op.socket);
    asio::async_write(*socket, buf,
        asio::bind_executor(write_strand_, [this, frame = std::move(op.frame), socket = std::move(op.socket)](const asio::error_code& ec, std::size_t) {
            if (ec) {
                spdlog::error("ChunkStoreService write error: {}", ec.message());
            }
            DoWrite();
        }));
}