#include "EntityStateStoreClient.h"
#include "core_generated.h"
#include "entity_state_store_generated.h"
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <cstring>
#include <arpa/inet.h>

namespace simcore {

EntityStateStoreClient::EntityStateStoreClient(asio::io_context& io)
    : io_(io), socket_(io), port_(0) {}

EntityStateStoreClient::~EntityStateStoreClient() { Disconnect(); }

void EntityStateStoreClient::Connect(const std::string& host, uint16_t port) {
    host_ = host; port_ = port;
    doConnect(host, port);
}

void EntityStateStoreClient::Disconnect() {
    stopped_ = true;
    if (socket_.is_open()) socket_.close();
    connected_ = false;
    pending_callbacks_.clear();
}

void EntityStateStoreClient::doConnect(const std::string& host, uint16_t port) {
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host), port);
    socket_.async_connect(endpoint, [this](asio::error_code ec) { onConnect(ec); });
}

void EntityStateStoreClient::onConnect(const asio::error_code& ec) {
    if (ec) {
        spdlog::error("EntityStateStoreClient connect error: {}", ec.message());
        connected_ = false;
        return;
    }
    connected_ = true;
    spdlog::info("EntityStateStoreClient connected to {}:{}", host_, port_);

    // Disable Nagle for low-latency RPC
    asio::ip::tcp::no_delay nodelay(true);
    asio::error_code opt_ec;
    socket_.set_option(nodelay, opt_ec);
    if (opt_ec) {
        spdlog::warn("Failed to set TCP_NODELAY on entity store socket: {}", opt_ec.message());
    }

    readFrame();
}

void EntityStateStoreClient::LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, uint16_t entity_type, LoadEntityStateCallback callback) {
    if (!connected_) { spdlog::error("EntityStateStoreClient not connected"); callback(EntityStateData{}); return; }
    flatbuffers::FlatBufferBuilder builder(1024);
    auto req = Protocol::CreateGetEntityStateReq(builder, dimension, x, y, z, entity_type);
    auto msg = Protocol::CreateEntityStateStoreMessage(builder, next_req_id_++, Protocol::EntityStateStoreRequest_GetEntityStateReq, req.Union());
    auto frame_offset = Protocol::CreateEntityStateStoreFrame(builder, Protocol::EntityStateStorePayload_EntityStateStoreMessage, msg.Union());
    builder.Finish(frame_offset);
    uint32_t req_id = next_req_id_ - 1;
    pending_callbacks_[req_id] = [callback](const std::vector<uint8_t>& data) {
        auto frame = Protocol::GetEntityStateStoreFrame(data.data());
        if (frame->payload_type() == Protocol::EntityStateStorePayload_EntityStateStoreReply) {
            auto reply = frame->payload_as_EntityStateStoreReply();
            if (reply->response_type() == Protocol::EntityStateStoreResponse_GetEntityStateResp) {
                auto resp = reply->response_as_GetEntityStateResp();
                auto state_vec = resp->state();
                EntityStateData result;
                if (state_vec) {
                    result.state.assign(state_vec->begin(), state_vec->end());
                }
                callback(result);
            } else { callback(EntityStateData{}); }
        } else { callback(EntityStateData{}); }
    };
    std::vector<uint8_t> frame;
    uint32_t size = builder.GetSize();
    frame.resize(4 + size);
    uint32_t be_size = htobe32(size);
    std::memcpy(frame.data(), &be_size, 4);
    std::memcpy(frame.data() + 4, builder.GetBufferPointer(), size);
    writeFrame(frame);
}

void EntityStateStoreClient::SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, uint16_t entity_type, const std::vector<uint8_t>& stateData, SaveEntityStateCallback callback) {
    if (!connected_) { spdlog::error("EntityStateStoreClient not connected"); callback(false); return; }
    flatbuffers::FlatBufferBuilder builder(1024);
    auto state_vec = builder.CreateVector(stateData);
    auto req = Protocol::CreateSetEntityStateReq(builder, dimension, x, y, z, entity_type, state_vec);
    auto msg = Protocol::CreateEntityStateStoreMessage(builder, next_req_id_++, Protocol::EntityStateStoreRequest_SetEntityStateReq, req.Union());
    auto frame_offset = Protocol::CreateEntityStateStoreFrame(builder, Protocol::EntityStateStorePayload_EntityStateStoreMessage, msg.Union());
    builder.Finish(frame_offset);
    uint32_t req_id = next_req_id_ - 1;
    pending_callbacks_[req_id] = [callback](const std::vector<uint8_t>& data) {
        auto frame = Protocol::GetEntityStateStoreFrame(data.data());
        if (frame->payload_type() == Protocol::EntityStateStorePayload_EntityStateStoreReply) {
            auto reply = frame->payload_as_EntityStateStoreReply();
            if (reply->response_type() == Protocol::EntityStateStoreResponse_EntityStateAck) {
                auto resp = reply->response_as_EntityStateAck();
                callback(resp->success());
            } else { callback(false); }
        } else { callback(false); }
    };
    std::vector<uint8_t> frame;
    uint32_t size = builder.GetSize();
    frame.resize(4 + size);
    uint32_t be_size = htobe32(size);
    std::memcpy(frame.data(), &be_size, 4);
    std::memcpy(frame.data() + 4, builder.GetBufferPointer(), size);
    writeFrame(frame);
}

void EntityStateStoreClient::writeFrame(const std::vector<uint8_t>& frame) {
    asio::async_write(socket_, asio::buffer(frame), [this](asio::error_code ec, size_t) {
        if (ec) { spdlog::error("EntityStateStoreClient write error: {}", ec.message()); connected_ = false; }
    });
}

void EntityStateStoreClient::readFrame() {
    auto header_buf = std::make_shared<std::vector<uint8_t>>(4);
    asio::async_read(socket_, asio::buffer(*header_buf), [this, header_buf](asio::error_code ec, size_t) {
        if (ec) { spdlog::error("EntityStateStoreClient read header error: {}", ec.message()); connected_ = false; return; }
        uint32_t size;
        std::memcpy(&size, header_buf->data(), 4);
        size = be32toh(size);
        auto payload_buf = std::make_shared<std::vector<uint8_t>>(size);
        asio::async_read(socket_, asio::buffer(*payload_buf), [this, payload_buf](asio::error_code ec, size_t) {
            if (ec) { spdlog::error("EntityStateStoreClient read payload error: {}", ec.message()); connected_ = false; return; }
            auto frame = Protocol::GetEntityStateStoreFrame(payload_buf->data());
            if (frame->payload_type() == Protocol::EntityStateStorePayload_EntityStateStoreReply) {
                auto reply = frame->payload_as_EntityStateStoreReply();
                uint32_t req_id = reply->req_id();
                auto it = pending_callbacks_.find(req_id);
                if (it != pending_callbacks_.end()) {
                    it->second(*payload_buf);
                    pending_callbacks_.erase(it);
                }
            }
            readFrame();
        });
    });
}

bool EntityStateStoreClient::IsConnected() const { return connected_; }

} // namespace simcore
