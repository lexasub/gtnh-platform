#include "IoUringChunkClient.h"
#include "core_generated.h"
#include "chunkstore_generated.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace simcore {

IoUringChunkClient::IoUringChunkClient()
    : tags_(tag_allocator_.alloc()) {}

IoUringChunkClient::~IoUringChunkClient() { Disconnect(); }

bool IoUringChunkClient::Connect(const std::string& host, uint16_t port) {
    if (connected_) return true;
    host_ = host; port_ = port;

    fd_ = gtnh::net::TcpConnector::connect(host.c_str(), port);
    if (fd_ < 0) {
        spdlog::error("IoUringChunkClient: connect to {}:{} failed", host, port);
        return false;
    }

    conn_ = std::make_unique<gtnh::net::IoUringConnection>(fd_, "chunkstore", tags_);

    conn_->on_message = [this](uint8_t msg_type, const uint8_t* data, size_t len) {
        onRead(msg_type, data, len);
    };
    conn_->on_closed = [this]() {
        connected_ = false;
        spdlog::warn("IoUringChunkClient: connection closed");
    };

    if (!conn_->start_reading()) {
        spdlog::error("IoUringChunkClient: failed to start reading");
        conn_->close();
        conn_.reset();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    connected_ = true;
    spdlog::info("IoUringChunkClient: connected to {}:{}", host, port);
    return true;
}

void IoUringChunkClient::Disconnect() {
    stopped_ = true;
    connected_ = false;
    if (conn_) {
        conn_->close();
        conn_.reset();
    }
}

bool IoUringChunkClient::IsConnected() const { return connected_; }

// =========================================================================
//  RPC methods — build FlatBuffer, prepend [4B BE size], send via send_raw
//  Wire protocol: [4B BE flatbuf_size][FlatBuf data]
// =========================================================================

void IoUringChunkClient::SetBlock(int32_t x, int32_t y, int32_t z,
                                   uint16_t block_id, uint8_t meta,
                                   SetBlockCallback callback) {
    if (!connected_) {
        spdlog::error("IoUringChunkClient: not connected");
        if (callback) callback(false);
        return;
    }

    flatbuffers::FlatBufferBuilder builder(1024);
    auto pos = Protocol::Vec3i(x, y, z);
    auto set_block_req = Protocol::CreateSetBlockReq(builder, &pos, block_id, meta);
    auto msg = Protocol::CreateChunkStoreMessage(builder, next_req_id_++,
        Protocol::ChunkStoreRequest_SetBlockReq, set_block_req.Union());
    auto frame_offset = Protocol::CreateChunkStoreFrame(builder,
        Protocol::ChunkStorePayload_ChunkStoreMessage, msg.Union());
    builder.Finish(frame_offset);

    uint32_t req_id = next_req_id_ - 1;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_callbacks_[req_id] = [callback](const std::vector<uint8_t>& data) {
            if (!callback) return;
            auto frame = Protocol::GetChunkStoreFrame(data.data());
            if (frame->payload_type() != Protocol::ChunkStorePayload_ChunkStoreReply) { callback(false); return; }
            auto reply = frame->payload_as_ChunkStoreReply();
            if (reply->response_type() != Protocol::ChunkStoreResponse_SetBlockResp) { callback(false); return; }
            auto resp = reply->response_as_SetBlockResp();
            callback(resp->success());
        };
    }

    std::vector<uint8_t> frame;
    uint32_t size = builder.GetSize();
    frame.resize(4 + size);
    uint32_t be_size = htobe32(size);
    std::memcpy(frame.data(), &be_size, 4);
    std::memcpy(frame.data() + 4, builder.GetBufferPointer(), size);
    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(frame));
    conn_->send_raw(shared);
}

void IoUringChunkClient::SetBlockCAS(int32_t x, int32_t y, int32_t z,
                                      uint16_t expected_block_id, uint16_t new_block_id, uint8_t meta,
                                      SetBlockCASCallback callback) {
    if (!connected_) {
        spdlog::error("IoUringChunkClient: not connected");
        if (callback) callback(CASResult{1, 0, 0});
        return;
    }

    flatbuffers::FlatBufferBuilder builder(1024);
    auto pos = Protocol::Vec3i(x, y, z);
    auto cas_req = Protocol::CreateSetBlockCASReq(builder, &pos, expected_block_id, new_block_id, meta);
    auto msg = Protocol::CreateChunkStoreMessage(builder, next_req_id_++,
        Protocol::ChunkStoreRequest_SetBlockCASReq, cas_req.Union());
    auto frame_offset = Protocol::CreateChunkStoreFrame(builder,
        Protocol::ChunkStorePayload_ChunkStoreMessage, msg.Union());
    builder.Finish(frame_offset);

    uint32_t req_id = next_req_id_ - 1;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_callbacks_[req_id] = [callback](const std::vector<uint8_t>& data) {
            if (!callback) return;
            auto frame = Protocol::GetChunkStoreFrame(data.data());
            if (frame->payload_type() != Protocol::ChunkStorePayload_ChunkStoreReply) { callback(CASResult{1,0,0}); return; }
            auto reply = frame->payload_as_ChunkStoreReply();
            if (reply->response_type() != Protocol::ChunkStoreResponse_SetBlockCASResp) { callback(CASResult{1,0,0}); return; }
            auto resp = reply->response_as_SetBlockCASResp();
            callback(CASResult{
                static_cast<uint8_t>(resp->status()),
                resp->actual_block_id(),
                resp->actual_meta()
            });
        };
    }

    std::vector<uint8_t> frame;
    uint32_t size = builder.GetSize();
    frame.resize(4 + size);
    uint32_t be_size = htobe32(size);
    std::memcpy(frame.data(), &be_size, 4);
    std::memcpy(frame.data() + 4, builder.GetBufferPointer(), size);
    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(frame));
    conn_->send_raw(shared);
}

void IoUringChunkClient::GetBlock(int32_t x, int32_t y, int32_t z,
                                   GetBlockCallback callback) {
    if (!connected_) {
        spdlog::error("IoUringChunkClient: not connected");
        if (callback) callback(BlockData{0, 0, 0});
        return;
    }

    flatbuffers::FlatBufferBuilder builder(1024);
    auto pos = Protocol::Vec3i(x, y, z);
    auto get_block_req = Protocol::CreateGetBlockReq(builder, &pos);
    auto msg = Protocol::CreateChunkStoreMessage(builder, next_req_id_++,
        Protocol::ChunkStoreRequest_GetBlockReq, get_block_req.Union());
    auto frame_offset = Protocol::CreateChunkStoreFrame(builder,
        Protocol::ChunkStorePayload_ChunkStoreMessage, msg.Union());
    builder.Finish(frame_offset);

    uint32_t req_id = next_req_id_ - 1;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_callbacks_[req_id] = [callback](const std::vector<uint8_t>& data) {
            if (!callback) return;
            auto frame = Protocol::GetChunkStoreFrame(data.data());
            if (frame->payload_type() != Protocol::ChunkStorePayload_ChunkStoreReply) { callback(BlockData{0,0,0}); return; }
            auto reply = frame->payload_as_ChunkStoreReply();
            if (reply->response_type() != Protocol::ChunkStoreResponse_GetBlockResp) { callback(BlockData{0,0,0}); return; }
            auto resp = reply->response_as_GetBlockResp();
            callback(BlockData{resp->block_id(), resp->meta(), resp->mb_id()});
        };
    }

    std::vector<uint8_t> frame;
    uint32_t size = builder.GetSize();
    frame.resize(4 + size);
    uint32_t be_size = htobe32(size);
    std::memcpy(frame.data(), &be_size, 4);
    std::memcpy(frame.data() + 4, builder.GetBufferPointer(), size);
    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(frame));
    conn_->send_raw(shared);
}

// =========================================================================
//  Read callback — called from IoUringConnection poll thread
// =========================================================================

void IoUringChunkClient::onRead(uint8_t msg_type, const uint8_t* data, size_t len) {
    (void)msg_type;
    if (stopped_) return;

    if (len < 4) {
        spdlog::warn("IoUringChunkClient: frame too short ({} bytes)", len);
        return;
    }

    auto frame = Protocol::GetChunkStoreFrame(data);
    if (frame->payload_type() != Protocol::ChunkStorePayload_ChunkStoreReply) return;
    auto reply = frame->payload_as_ChunkStoreReply();
    uint32_t req_id = reply->req_id();

    std::function<void(const std::vector<uint8_t>&)> cb;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_callbacks_.find(req_id);
        if (it != pending_callbacks_.end()) {
            cb = std::move(it->second);
            pending_callbacks_.erase(it);
        }
    }

    if (!cb) {
        spdlog::warn("IoUringChunkClient: no callback for req_id={}", req_id);
        return;
    }

    auto payload = std::make_shared<std::vector<uint8_t>>(data, data + len);
    cb(*payload);
}

} // namespace simcore
