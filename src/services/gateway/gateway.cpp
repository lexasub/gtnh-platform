// gateway.cpp
#include "gateway.h"
#include "gateway_generated.h"
#include "quest_generated.h"

#include <gtnh/net/frame.h>
#include <flatbuffers/flatbuffers.h>

#include <spdlog/spdlog.h>

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
IoUringGateway::~IoUringGateway() { shutdown(); }

// =========================================================================
//  Lifecycle
// =========================================================================

bool IoUringGateway::init() {
    // TcpServer and RouterClient self-initialize on listen()/connect()
    return true;
}

// =========================================================================
//  TCP accept (ctrl + bulk)
// =========================================================================

bool IoUringGateway::listen(uint16_t ctrl_port, uint16_t bulk_port) {
    ctrl_port_ = ctrl_port;
    bulk_port_ = bulk_port;

    ctrl_server_.on_accept = [this](int client_fd) {
        auto tags = tag_alloc_.alloc();
        auto conn = std::make_unique<gtnh::net::IoUringConnection>(
            client_fd, "ctrl", tags);
        conn->on_message = [this](uint8_t type, const uint8_t* d, size_t l) {
            on_client_ctrl_message(type, d, l);
        };
        auto sess = ++session_gen_;
        conn->on_closed = [this, sess]() {
            spdlog::info("Gateway: ctrl client disconnected");
            {
                std::lock_guard<std::mutex> lock(client_state_mutex_);
                // Only act if no newer session has started — the old on_closed
                // callback may fire after the next accept handler has already
                // set up state for a new session (race on rapid reconnect).
                if (session_gen_.load() != sess) {
                    spdlog::info("Gateway: stale on_closed from session {} skipped", sess);
                    return;
                }
                if (player_id_known_) {
                    flatbuffers::FlatBufferBuilder fbb;
                    auto left = Protocol::CreatePlayerLeft(
                        fbb, client_player_id_, last_x_, last_y_, last_z_);
                    fbb.Finish(left);
                    publish("player.left", fbb.GetBufferPointer(), fbb.GetSize());
                    spdlog::info("Gateway: player left (id={}, pos=[{}, {}, {}])",
                                 client_player_id_, last_x_, last_y_, last_z_);
                }
                player_id_known_ = false;
            }
        };
        if (!conn->start_reading()) {
            spdlog::error("Gateway: failed to start ctrl read loop");
            conn->close();
            return;
        }
        // Same pattern as bulk: always accept, destroy old outside lock.
        std::unique_ptr<gtnh::net::IoUringConnection> old_conn;
        {
            std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
            old_conn = std::move(client_ctrl_);
            client_ctrl_ = std::move(conn);
        }
        // old_conn destroyed here (outside lock)
        spdlog::info("Gateway: ctrl client connected");
        
        // Reset player state for new connection (old on_closed may not have
        // fired yet, leaving player_id_known_ = true from prior session).
        // Publish player.joined eagerly so MetaDB pushes inventory immediately
        // — avoids race between client's first message and old on_closed.
        {
            std::lock_guard<std::mutex> lock(client_state_mutex_);
            player_id_known_ = true;
            client_player_id_ = 1;      // matches client's hardcoded dev ID
            last_x_ = last_x_ ? last_x_ : 256;
            if (!last_y_) last_y_ = 80;
            last_z_ = last_z_ ? last_z_ : 224;
            flatbuffers::FlatBufferBuilder fbb;
            auto joined = Protocol::CreatePlayerJoined(fbb, 1);
            fbb.Finish(joined);
            publish("player.joined", fbb.GetBufferPointer(), fbb.GetSize());
            spdlog::info("Gateway: player joined (id=1) on connect");
        }

        // Trigger CHUNK_REQUEST to load initial chunk
        // Use last known position from previous session, or default spawn (0,50,0)
        {
            std::lock_guard<std::mutex> lock(client_state_mutex_);
            int32_t sx = last_x_ ? last_x_ : 0;
            int32_t sy = last_y_ ? last_y_ : 50;
            int32_t sz = last_z_ ? last_z_ : 0;
            int32_t cx = sx >> 5, cy = sy >> 5, cz = sz >> 5;
            flatbuffers::FlatBufferBuilder fbb;
            auto pos = Protocol::Vec3i(cx, cy, cz);
            auto action = Protocol::CreatePlayerAction(fbb, 0,
                                                       Protocol::PlayerActionType_CHUNK_REQUEST, &pos);
            fbb.Finish(action);
            spdlog::info("Gateway: sending CHUNK_REQUEST at chunk ({},{},{})", cx, cy, cz);
            publish("player.actions", fbb.GetBufferPointer(), fbb.GetSize());
        }
    };

    bulk_server_.on_accept = [this](int client_fd) {
        auto tags = tag_alloc_.alloc();
        auto conn = std::make_unique<gtnh::net::IoUringConnection>(
            client_fd, "bulk", tags);
        conn->on_message = [this](uint8_t type, const uint8_t* d, size_t l) {
            on_client_bulk_message(type, d, l);
        };
        conn->on_closed = [this]() {
            spdlog::info("Gateway: bulk client disconnected");
        };
        if (!conn->start_reading()) {
            spdlog::error("Gateway: failed to start bulk read loop");
            conn->close();
            return;
        }
        // Always accept new connection.  Swap the old out first so its
        // destructor (which joins the poll thread) runs outside the lock
        // and doesn't block the TcpServer accept handler or other threads.
        std::unique_ptr<gtnh::net::IoUringConnection> old_conn;
        {
            std::lock_guard<std::mutex> lock(client_bulk_mutex_);
            old_conn = std::move(client_bulk_);
            client_bulk_ = std::move(conn);
        }
        // old_conn destroyed here (outside lock) — joining its poll thread
        // may block for up to 50 ms (the poll timeout).  Acceptable during
        // reconnect.
        spdlog::info("Gateway: bulk client connected");
    };

    if (!ctrl_server_.listen(ctrl_port, "ctrl")) return false;
    if (!bulk_server_.listen(bulk_port, "bulk")) return false;
    return true;
}

// =========================================================================
//  Router connection
// =========================================================================

bool IoUringGateway::connect_router(const std::string& host, uint16_t port) {
    router_.on_publish = [this](const std::string& topic,
                                 std::shared_ptr<std::vector<uint8_t>> data) {
        on_router_publish(topic, std::move(data));
    };
    return router_.connect(host.c_str(), port, "gateway");
}

// =========================================================================
//  Router protocol
// =========================================================================

void IoUringGateway::sendHeartbeat() {
    router_.heartbeat();
}

void IoUringGateway::subscribe(const std::string& topic) {
    router_.subscribe(topic);
}

void IoUringGateway::publish(const std::string& topic, const uint8_t* data, size_t len) {
    router_.publish(topic, data, len);
}

// =========================================================================
//  Client I/O (Ctrl = small msgs, Bulk = large data)
// =========================================================================

void IoUringGateway::send_to_client_ctrl(uint8_t msg_type, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
    if (client_ctrl_ && client_ctrl_->is_open())
        client_ctrl_->send(msg_type, data, len);
}

void IoUringGateway::send_to_client_bulk(uint8_t msg_type, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(client_bulk_mutex_);
    if (client_bulk_ && client_bulk_->is_open())
        client_bulk_->send(msg_type, data, len);
}

void IoUringGateway::send_to_client_ctrl_raw(std::shared_ptr<std::vector<uint8_t>> frame) {
    std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
    if (client_ctrl_ && client_ctrl_->is_open())
        client_ctrl_->send_raw(std::move(frame));
}

void IoUringGateway::send_to_client_bulk_raw(std::shared_ptr<std::vector<uint8_t>> frame) {
    std::lock_guard<std::mutex> lock(client_bulk_mutex_);
    if (client_bulk_ && client_bulk_->is_open())
        client_bulk_->send_raw(std::move(frame));
}

void IoUringGateway::send_to_client_ctrl_raw(uint8_t msg_type, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
    if (client_ctrl_ && client_ctrl_->is_open())
        client_ctrl_->send(msg_type, data, len);
}

bool IoUringGateway::send_to_client_bulk_raw(uint8_t msg_type, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(client_bulk_mutex_);
    if (!client_bulk_) { spdlog::warn("Gateway: bulk client null, dropping {} bytes", len); return false; }
    if (!client_bulk_->is_open()) { spdlog::warn("Gateway: bulk client closed, dropping {} bytes", len); return false; }
    // Build a test frame to log its first bytes before sending
    auto test_frame = gtnh::net::frame::pack(msg_type, data, len);
    spdlog::info("Gateway: SEND bulk type={} len={} frame_size={} first_bytes={:02x} {:02x} {:02x} {:02x} {:02x}",
                  msg_type, len, test_frame->size(),
                  test_frame->size() > 0 ? (*test_frame)[0] : 0,
                  test_frame->size() > 1 ? (*test_frame)[1] : 0,
                  test_frame->size() > 2 ? (*test_frame)[2] : 0,
                  test_frame->size() > 3 ? (*test_frame)[3] : 0,
                  test_frame->size() > 4 ? (*test_frame)[4] : 0);
    client_bulk_->send_raw(std::move(test_frame));
    return true;
}

PlayerInterest* IoUringGateway::client_interest() { return nullptr; }

bool IoUringGateway::has_client() const {
    std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
    return client_ctrl_ && client_ctrl_->is_open();
}

void IoUringGateway::publish_player_joined() {
    std::lock_guard<std::mutex> lock(client_state_mutex_);
    if (!player_id_known_) return;
    flatbuffers::FlatBufferBuilder fbb;
    auto joined = Protocol::CreatePlayerJoined(fbb, client_player_id_);
    fbb.Finish(joined);
    publish("player.joined", fbb.GetBufferPointer(), fbb.GetSize());
    spdlog::info("Gateway: republished player.join (id={}) on upstream reconnect", client_player_id_);
}

void IoUringGateway::publish_player_left() {
    std::lock_guard<std::mutex> lock(client_state_mutex_);
    if (!player_id_known_) return;
    flatbuffers::FlatBufferBuilder fbb;
    auto left = Protocol::CreatePlayerLeft(
        fbb, client_player_id_, last_x_, last_y_, last_z_);
    fbb.Finish(left);
    publish("player.left", fbb.GetBufferPointer(), fbb.GetSize());
    spdlog::info("Gateway: publish player.left (id={}, pos=[{}, {}, {}])",
                 client_player_id_, last_x_, last_y_, last_z_);
}

// =========================================================================
//  Shutdown
// =========================================================================

void IoUringGateway::shutdown() {
    spdlog::info("Gateway: shutting down...");
    // Flush player position before disconnecting from router — after
    // router_.disconnect() the publish would silently fail and the last
    // position would be lost.
    publish_player_left();
    router_.disconnect();
    {
        std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
        if (client_ctrl_) { client_ctrl_->close(); client_ctrl_.reset(); }
    }
    {
        std::lock_guard<std::mutex> lock(client_bulk_mutex_);
        if (client_bulk_) { client_bulk_->close(); client_bulk_.reset(); }
    }
    ctrl_server_.stop();
    bulk_server_.stop();
    spdlog::info("Gateway: stopped");
}

// =========================================================================
//  Router publish callback
// =========================================================================

void IoUringGateway::on_router_publish(
    const std::string& topic, std::shared_ptr<std::vector<uint8_t>> data)
{
    const uint8_t* payload = data->data();
    size_t plen = data->size();
    // DEBUG: log every router publish to identify source of 5 zero bytes
    if (plen < 8) {
        spdlog::warn("Gateway: RX topic='{}' len={} (SHORT!) first_bytes={:02x} {:02x} {:02x} {:02x}",
                      topic, plen,
                      plen > 0 ? payload[0] : 0,
                      plen > 1 ? payload[1] : 0,
                      plen > 2 ? payload[2] : 0,
                      plen > 3 ? payload[3] : 0);
    }
    spdlog::debug("Gateway: RX topic='{}' len={}", topic, plen);

    // Bulk topics → bulk connection
    if (topic == "world.chunk.loaded.compressed") {
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::CompressedChunkData>(nullptr)) {
            bool sent = send_to_client_bulk_raw(GatewayMsg::kCompressedChunkData, payload, plen);
            spdlog::info("Gateway: compressed chunk received, bulk_sent={}", sent);
            send_to_client_ctrl_raw(GatewayMsg::kCompressedChunkData, payload, plen);
        } else
            spdlog::warn("Gateway: Router: invalid CompressedChunkData FlatBuffer");
    } else if (topic == "world.blocks.changed") {
        send_to_client_bulk_raw(GatewayMsg::kBlockUpdate, payload, plen);
    } else if (topic.find("entities.") == 0) {
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::EntitySnapshot>(nullptr))
            send_to_client_bulk_raw(GatewayMsg::kEntitySnapshot, payload, plen);
        else
            spdlog::warn("Gateway: Router: invalid EntitySnapshot FlatBuffer");
    } else if (topic == "simulation.multiblock.created") {
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::MultiblockCreatedEvent>(nullptr))
            send_to_client_bulk_raw(GatewayMsg::kEntitySnapshot, payload, plen);
        else
            spdlog::warn("Gateway: Router: invalid MultiblockCreatedEvent");
    } else if (topic == "simulation.multiblock.destroyed") {
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::MultiblockDestroyedEvent>(nullptr))
            send_to_client_bulk_raw(GatewayMsg::kEntitySnapshot, payload, plen);
        else
            spdlog::warn("Gateway: Router: invalid MultiblockDestroyedEvent");
    }
    // Ctrl topics → ctrl connection
    else if (topic == "player.actions.ack")
        send_to_client_ctrl_raw(GatewayMsg::kBlockAck, payload, plen);
    else if (topic == "player.inventory.update")
        send_to_client_ctrl_raw(GatewayMsg::kInventoryUpdate, payload, plen);
    else if (topic == "world.block_entity.update")
        send_to_client_ctrl_raw(GatewayMsg::kBlockEntityUpdate, payload, plen);
    else if (topic == "sim.craft.response")
        send_to_client_ctrl_raw(GatewayMsg::kCraftResponse, payload, plen);
else if (topic == "player.machine.slot.response")
    send_to_client_ctrl_raw(GatewayMsg::kSetMachineSlotResp, payload, plen);
else if (topic == "player.chest.open.response")
    send_to_client_ctrl_raw(GatewayMsg::kChestOpenResp, payload, plen);
    else if (topic == "player.tool.action.response")
        send_to_client_ctrl_raw(GatewayMsg::kToolActionResp, payload, plen);
    else if (topic == "player.position.load") {
        auto pos = flatbuffers::GetRoot<Protocol::PlayerLeft>(payload);
        if (pos) {
            std::lock_guard<std::mutex> lock(client_state_mutex_);
            last_x_ = pos->x(); last_y_ = pos->y(); last_z_ = pos->z();
            spdlog::info("Gateway: position restored ([{}, {}, {}])",
                         last_x_, last_y_, last_z_);
            int32_t cx = last_x_ >> 5, cy = last_y_ >> 5, cz = last_z_ >> 5;
            flatbuffers::FlatBufferBuilder fbb;
            auto cp = Protocol::Vec3i(cx, cy, cz);
            auto action = Protocol::CreatePlayerAction(fbb, 0,
                                                       Protocol::PlayerActionType_CHUNK_REQUEST, &cp);
            fbb.Finish(action);
            publish("player.actions", fbb.GetBufferPointer(), fbb.GetSize());
            spdlog::info("Gateway: re-sent CHUNK_REQUEST at chunk ({},{},{})", cx, cy, cz);
        }
    }
    else if (topic == "quest.progress.updated")
        send_to_client_ctrl_raw(GatewayMsg::kQuestProgressUpdate, payload, plen);
    else if (topic == "quest.unlocked")
        send_to_client_ctrl_raw(GatewayMsg::kQuestUnlockNotification, payload, plen);
    else if (topic == "quest.completed")
        send_to_client_ctrl_raw(GatewayMsg::kQuestCompletedNotification, payload, plen);
    else if (on_router_message)
        on_router_message(topic, payload, plen);
}

// =========================================================================
//  Client read callbacks
// =========================================================================

void IoUringGateway::on_client_ctrl_message(uint8_t msg_type, const uint8_t* data, size_t len) {
    switch (msg_type) {
    case GatewayMsg::kPlayerAction: {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifyBuffer<Protocol::PlayerAction>(nullptr)) { spdlog::error("Gateway: invalid PlayerAction on ctrl"); return; }
        if (on_client_message) on_client_message(data, len);
        auto action = flatbuffers::GetRoot<Protocol::PlayerAction>(data);
        if (!action) return;
        uint64_t pid = action->player_id();
        std::lock_guard<std::mutex> lock(client_state_mutex_);
        if (!player_id_known_) {
            client_player_id_ = pid;
            player_id_known_ = true;
            flatbuffers::FlatBufferBuilder fbb;
            auto joined = Protocol::CreatePlayerJoined(fbb, pid);
            fbb.Finish(joined);
            const uint8_t* d = fbb.GetBufferPointer();
            size_t s = fbb.GetSize();
            publish("player.joined", d, s);
            spdlog::info("Gateway: player joined (id={})", pid);
        }
        auto pos = action->pos();
        last_x_ = pos->x(); last_y_ = pos->y(); last_z_ = pos->z();
        return;
    }
    case GatewayMsg::kSetBlockAction: {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifyBuffer<Protocol::SetBlockAction>(nullptr)) { spdlog::error("Gateway: invalid SetBlockAction on ctrl"); return; }
        if (on_client_message) on_client_message(data, len);
        auto action = flatbuffers::GetRoot<Protocol::SetBlockAction>(data);
        if (!action) return;
        uint64_t pid = action->player_id();
        {
            std::lock_guard<std::mutex> lock(client_state_mutex_);
            if (!player_id_known_) {
                client_player_id_ = pid;
                player_id_known_ = true;
                flatbuffers::FlatBufferBuilder fbb;
                auto joined = Protocol::CreatePlayerJoined(fbb, pid);
                fbb.Finish(joined);
                const uint8_t* d = fbb.GetBufferPointer();
                size_t s = fbb.GetSize();
                publish("player.joined", d, s);
                spdlog::info("Gateway: player joined (id={})", pid);
            }
            auto pos = action->pos();
            if (pos) { last_x_ = pos->x(); last_y_ = pos->y(); last_z_ = pos->z(); }
        }
        return;
    }
    case GatewayMsg::kInventoryAction: {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifyBuffer<Protocol::InventoryAction>(nullptr)) { spdlog::error("Gateway: invalid InventoryAction on ctrl"); return; }
        publish("player.inventory.actions", data, len);
        break;
    }
    case GatewayMsg::kCraftRequest: {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifyBuffer<Protocol::CraftRequest>(nullptr)) { spdlog::error("Gateway: invalid CraftRequest on ctrl"); return; }
        publish("sim.craft.request", data, len);
        break;
    }
    case GatewayMsg::kSetMachineSlot: {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifyBuffer<Protocol::SetMachineSlotReq>(nullptr)) { spdlog::error("Gateway: invalid SetMachineSlotReq on ctrl"); return; }
        publish("player.machine.slot", data, len);
        break;
    }
    case GatewayMsg::kChestOpenReq: {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifyBuffer<Protocol::ChestOpenReq>(nullptr)) { spdlog::error("Gateway: invalid ChestOpenReq on ctrl"); return; }
        publish("player.chest.open", data, len);
        break;
    }
    case GatewayMsg::kToolAction: {
        flatbuffers::Verifier v(data, len);
        if (!v.VerifyBuffer<Protocol::ToolAction>(nullptr)) { spdlog::error("Gateway: invalid ToolAction on ctrl"); return; }
        publish("player.tool.action", data, len);
        break;
    }
    default: spdlog::warn("Gateway: unknown ctrl client msg type {}", msg_type); break;
    }
}

void IoUringGateway::on_client_bulk_message([[maybe_unused]] uint8_t msg_type, [[maybe_unused]] const uint8_t* data, size_t len) {
    spdlog::warn("Gateway: bulk client sent msg_type={} len={} (unexpected on bulk)", msg_type, len);
}
