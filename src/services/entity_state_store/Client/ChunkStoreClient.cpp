#include "Client/ChunkStoreClient.h"
#include "core_generated.h"
#include "chunkstore_generated.h"
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <cstring>
#include <arpa/inet.h>

namespace gtnh {
namespace entity_state_store {

ChunkStoreClient::ChunkStoreClient(asio::io_context& io)
    : io_(io), socket_(io), port_(0) {}

ChunkStoreClient::~ChunkStoreClient() { Disconnect(); }

void ChunkStoreClient::Connect(const std::string& host, uint16_t port) {
    host_ = host; port_ = port;
    doConnect(host, port);
}

void ChunkStoreClient::Disconnect() {
    stopped_ = true;
    if (socket_.is_open()) socket_.close();
    connected_ = false;
}

void ChunkStoreClient::doConnect(const std::string& host, uint16_t port) {
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host), port);
    socket_.async_connect(endpoint, [this](asio::error_code ec) { onConnect(ec); });
}

void ChunkStoreClient::onConnect(const asio::error_code& ec) {
    if (ec) {
        spdlog::error("ChunkStoreClient connect error: {}", ec.message());
        connected_ = false;
        return;
    }
    connected_ = true;
    spdlog::info("ChunkStoreClient connected to {}:{}", host_, port_);
    readFrame();
}

void ChunkStoreClient::SetBlock(int32_t x, int32_t y, int32_t z, uint16_t block_id, uint8_t meta, SetBlockCallback callback) {
    if (!connected_) { spdlog::error("ChunkStoreClient not connected"); callback(false); return; }
    flatbuffers::FlatBufferBuilder builder(1024);
    auto pos = Protocol::Vec3i(x, y, z);
    auto set_block_req = Protocol::CreateSetBlockReq(builder, &pos, block_id, meta);
    auto msg = Protocol::CreateChunkStoreMessage(builder, next_req_id_++, Protocol::ChunkStoreRequest_SetBlockReq, set_block_req.Union());
    auto frame_offset = Protocol::CreateChunkStoreFrame(builder, Protocol::ChunkStorePayload_ChunkStoreMessage, msg.Union());
    builder.Finish(frame_offset);
    uint32_t req_id = next_req_id_ - 1;
    pending_callbacks_[req_id] = [callback](const std::vector<uint8_t>& data) {
        auto frame = Protocol::GetChunkStoreFrame(data.data());
        if (frame->payload_type() != Protocol::ChunkStorePayload_ChunkStoreReply) { callback(false); return; }
        auto reply = frame->payload_as_ChunkStoreReply();
        if (reply->response_type() != Protocol::ChunkStoreResponse_SetBlockResp) { callback(false); return; }
        auto resp = reply->response_as_SetBlockResp();
        callback(resp->success());
    };
    std::vector<uint8_t> frame;
    uint32_t size = builder.GetSize();
    frame.resize(4 + size);
    uint32_t be_size = htobe32(size);
    std::memcpy(frame.data(), &be_size, 4);
    std::memcpy(frame.data() + 4, builder.GetBufferPointer(), size);
    writeFrame(frame);
}

void ChunkStoreClient::GetBlock(int32_t x, int32_t y, int32_t z, GetBlockCallback callback) {
    if (!connected_) { spdlog::error("ChunkStoreClient not connected"); callback(BlockData{0,0,0}); return; }
    flatbuffers::FlatBufferBuilder builder(1024);
    auto pos = Protocol::Vec3i(x, y, z);
    auto get_block_req = Protocol::CreateGetBlockReq(builder, &pos);
    auto msg = Protocol::CreateChunkStoreMessage(builder, next_req_id_++, Protocol::ChunkStoreRequest_GetBlockReq, get_block_req.Union());
    auto frame_offset = Protocol::CreateChunkStoreFrame(builder, Protocol::ChunkStorePayload_ChunkStoreMessage, msg.Union());
    builder.Finish(frame_offset);
    uint32_t req_id = next_req_id_ - 1;
    pending_callbacks_[req_id] = [callback](const std::vector<uint8_t>& data) {
        auto frame = Protocol::GetChunkStoreFrame(data.data());
        if (frame->payload_type() != Protocol::ChunkStorePayload_ChunkStoreReply) { callback(BlockData{0,0,0}); return;}
        auto reply = frame->payload_as_ChunkStoreReply();
        if (reply->response_type() != Protocol::ChunkStoreResponse_GetBlockResp) { callback(BlockData{0,0,0}); return; }
        auto resp = reply->response_as_GetBlockResp();
        callback(BlockData{resp->block_id(), resp->meta(), resp->mb_id()});
    };
    std::vector<uint8_t> frame;
    uint32_t size = builder.GetSize();
    frame.resize(4 + size);
    uint32_t be_size = htobe32(size);
    std::memcpy(frame.data(), &be_size, 4);
    std::memcpy(frame.data() + 4, builder.GetBufferPointer(), size);
    writeFrame(frame);
}

void ChunkStoreClient::writeFrame(const std::vector<uint8_t>& frame) {
    asio::async_write(socket_, asio::buffer(frame), [this](asio::error_code ec, size_t) {
        if (ec) { spdlog::error("ChunkStoreClient write error: {}", ec.message()); connected_ = false; }
    });
}

void ChunkStoreClient::readFrame() {
    auto header_buf = std::make_shared<std::vector<uint8_t>>(4);
    asio::async_read(socket_, asio::buffer(*header_buf), [this, header_buf](asio::error_code ec, size_t) {
        if (ec) { spdlog::error("ChunkStoreClient read header error: {}", ec.message()); connected_ = false; return; }
        uint32_t size;
        std::memcpy(&size, header_buf->data(), 4);
        size = be32toh(size);
        auto payload_buf = std::make_shared<std::vector<uint8_t>>(size);
        asio::async_read(socket_, asio::buffer(*payload_buf), [this, payload_buf](asio::error_code ec, size_t) {
            if (ec) { spdlog::error("ChunkStoreClient read payload error: {}", ec.message()); connected_ = false; return; }
            auto frame = Protocol::GetChunkStoreFrame(payload_buf->data());
            if (frame->payload_type() != Protocol::ChunkStorePayload_ChunkStoreReply) {
                return;
            }
            auto reply = frame->payload_as_ChunkStoreReply();
            uint32_t req_id = reply->req_id();
            auto it = pending_callbacks_.find(req_id);
            if (it != pending_callbacks_.end()) {
                it->second(*payload_buf);
                pending_callbacks_.erase(it);
            }
            readFrame();
        });
    });
}

bool ChunkStoreClient::IsConnected() const { return connected_; }

} // namespace entity_state_store
} // namespace gtnh
