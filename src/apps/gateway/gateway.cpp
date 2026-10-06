// gateway.cpp
#include "gateway.h"
#include "client_state_generated.h"
#include "gateway_generated.h"
#include "pipe_network_generated.h"
#include "quest_generated.h"
#include "recipe_generated.h"
#include "service_health_generated.h"
#include "common/FlatBuffersVerify.h"
#include "common/ResourceBufferStateCodec.h"

#include <gtnh/net/frame.h>
#include <flatbuffers/flatbuffers.h>

#include <spdlog/spdlog.h>

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <shared_mutex>

IoUringGateway::~IoUringGateway() { shutdown(); }

// =========================================================================
//  TCP accept (ctrl + bulk)
//
//  Two sockets, two sessions. The ctrl socket owns the session: it gets an
//  identity and is the lifetime the session lives and dies with. The bulk
//  socket is push-only and pairs itself to a session with the session id the
//  client received in its welcome — no guessing by IP or arrival order, so
//  several clients can have their bulk sockets open simultaneously.
// =========================================================================

bool IoUringGateway::listen(uint16_t ctrl_port, uint16_t bulk_port) {
    ctrl_port_ = ctrl_port;
    ctrl_server_.on_accept = [this](int client_fd) {
        auto tags = tag_alloc_.alloc();
        auto conn = std::make_shared<gtnh::net::IoUringConnection>(
            client_fd, "ctrl", tags);
        auto ctx = std::make_shared<ClientConnContext>();

        conn->on_message = [this, ctx](uint8_t type, const uint8_t* d, size_t l) {
            const uint64_t sid = ctx->session_id.load(std::memory_order_acquire);
            if (sid == 0) return; // session already torn down
            on_client_ctrl_message(sid, type, d, l);
        };
        conn->on_closed = [this, ctx]() {
            // exchange(0) makes teardown idempotent: a session is removed by
            // exactly one of its sockets' close callbacks, whichever fires
            // first.
            const uint64_t sid = ctx->session_id.exchange(0, std::memory_order_acq_rel);
            if (sid == 0) return;
            // Only the registry mutation happens under conn_mutex_: publishing
            // player.left goes to the router over the network, and holding the
            // lock across that would stall every send in the process if the
            // router stalls.
            std::shared_ptr<gtnh::net::ClientSession> session;
            std::shared_ptr<gtnh::net::IoUringConnection> ctrl_conn;
            {
                std::unique_lock<std::shared_mutex> guard(conn_mutex_);
                session = sessions_.remove_session(sid);
                if (session) ctrl_conn = std::move(session->ctrl);
            }
            if (session) {
                announce_session_left(session);
                retire(std::move(ctrl_conn));
            }
            spdlog::info("Gateway: ctrl session {} disconnected ({} sessions left)",
                         sid, sessions_.size());
        };

        // Assign identity before any byte can arrive, so the first client
        // message is already attributed to a session.
        std::shared_ptr<gtnh::net::ClientSession> session;
        {
            std::unique_lock<std::shared_mutex> guard(conn_mutex_);
            session = sessions_.create_session(conn);
        }
        ctx->session_id.store(session->session_id, std::memory_order_release);

        if (!conn->start_reading()) {
            spdlog::error("Gateway: failed to start ctrl read loop");
            ctx->session_id.store(0, std::memory_order_release);
            std::unique_lock<std::shared_mutex> guard(conn_mutex_);
            if (auto doomed = sessions_.remove_session(session->session_id))
                retire(std::move(doomed->ctrl));
            return;
        }

        // The welcome is the client's only source of identity: everything it
        // sends afterwards must carry this player_id, and the gateway drops
        // frames that claim a different one.
        flatbuffers::FlatBufferBuilder fbb;
        auto welcome = Protocol::CreateSessionWelcome(fbb, session->session_id,
                                                      session->player_id);
        fbb.Finish(welcome);
        conn->send(GatewayMsg::kSessionWelcome, fbb.GetBufferPointer(), fbb.GetSize());
        spdlog::info("Gateway: ctrl session {} joined as player {} ({} sessions)",
                     session->session_id, session->player_id, sessions_.size());

        announce_session_join(session);
    };

    bulk_server_.on_accept = [this](int client_fd) {
        auto tags = tag_alloc_.alloc();
        auto conn = std::make_shared<gtnh::net::IoUringConnection>(
            client_fd, "bulk", tags);
        auto ctx = std::make_shared<ClientConnContext>();
        // Parked under a token, not in the context: the connection owns the
        // callbacks holding the context, so a context that owned the
        // connection back would be a cycle leaking a poll thread per client.
        ctx->bulk_token = next_bulk_token_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(pending_bulk_mutex_);
            pending_bulk_[ctx->bulk_token] = conn;
        }

        conn->on_message = [this, ctx](uint8_t type, const uint8_t* d, size_t l) {
            on_client_bulk_message(ctx, type, d, l);
        };
        conn->on_closed = [this, ctx]() {
            const uint64_t sid = ctx->session_id.exchange(0, std::memory_order_acq_rel);
            if (sid == 0) {
                // Never paired: forget the parked connection, but destroy it on
                // the main loop — see IoUringGateway::retire.
                if (auto unpaired = take_pending_bulk(ctx->bulk_token))
                    retire(std::move(unpaired));
                spdlog::info("Gateway: unpaired bulk connection closed");
                return;
            }
            std::shared_ptr<gtnh::net::IoUringConnection> bulk_conn;
            std::unique_lock<std::shared_mutex> guard(conn_mutex_);
            if (auto session = sessions_.detach_bulk(sid, &bulk_conn)) {
                // Taken, not reset in place: the registry dropping the last
                // reference here would destroy the connection on its own poll
                // thread (see retire).
                retire(std::move(bulk_conn));
                spdlog::info("Gateway: bulk socket detached from session {}", sid);
            }
        };

        if (!conn->start_reading()) {
            spdlog::error("Gateway: failed to start bulk read loop");
            if (auto doomed = take_pending_bulk(ctx->bulk_token)) {
                doomed->close();
                retire(std::move(doomed));
            }
            return;
        }
        spdlog::info("Gateway: bulk connection accepted, awaiting SessionHello");
    };

    if (!ctrl_server_.listen(ctrl_port, "ctrl")) return false;
    if (!bulk_server_.listen(bulk_port, "bulk")) return false;
    return true;
}

// =========================================================================
//  Router connection
// =========================================================================

void IoUringGateway::configure_router_callbacks() {
    router_.on_publish = [this](const std::string& topic,
                                 std::shared_ptr<std::vector<uint8_t>> data) {
        on_router_publish(topic, std::move(data));
    };
    router_.set_health_response_callback(
        [this](std::shared_ptr<std::vector<uint8_t>> data) {
            if (!data || data->size() < 10) return;
            const auto &raw = *data;
            uint64_t nonce = 0;
            for (size_t i = 0; i < sizeof(nonce); ++i)
                nonce = (nonce << 8) | raw[i];
            const uint32_t request_id = static_cast<uint32_t>(nonce >> 32);
            size_t offset = sizeof(nonce);
            const uint16_t count = static_cast<uint16_t>(raw[offset] << 8 | raw[offset + 1]);
            offset += 2;
            flatbuffers::FlatBufferBuilder builder(256);
            std::vector<flatbuffers::Offset<Protocol::ServiceHealth>> rows;
            rows.reserve(count);
            for (uint16_t i = 0; i < count; ++i) {
                if (offset + 2 > raw.size()) return;
                const uint16_t name_len = static_cast<uint16_t>(raw[offset] << 8 | raw[offset + 1]);
                offset += 2;
                if (offset + name_len + 10 > raw.size()) return;
                auto name = builder.CreateString(reinterpret_cast<const char *>(raw.data() + offset), name_len);
                offset += name_len;
                const auto state = static_cast<Protocol::ServiceHealthProbeState>(raw[offset++]);
                const bool transport = raw[offset++] != 0;
                uint64_t age = 0;
                for (size_t j = 0; j < sizeof(age); ++j) age = (age << 8) | raw[offset++];
                rows.push_back(Protocol::CreateServiceHealth(builder, name, transport, state, age));
            }
            auto vector = builder.CreateVector(rows);
            auto response = Protocol::CreateServiceHealthResp(builder, request_id, vector);
            builder.Finish(response);
            broadcast_ctrl(GatewayMsg::kServiceHealthResp,
                           builder.GetBufferPointer(), builder.GetSize());
        });
}

bool IoUringGateway::connect_router(const std::string& host, uint16_t port) {
    router_host_ = host;
    router_port_ = port;
    configure_router_callbacks();
    return router_.connect(host.c_str(), port, "gateway");
}

bool IoUringGateway::connect_router() {
    if (router_host_.empty()) {
        spdlog::error("Gateway: connect_router() called before initial connect");
        return false;
    }
    spdlog::info("Gateway: reconnecting to router {}:{}", router_host_, router_port_);
    configure_router_callbacks();
    return router_.connect(router_host_.c_str(), router_port_, "gateway");
}

// =========================================================================
//  Router protocol
// =========================================================================

void IoUringGateway::sendHeartbeat() {
    router_.heartbeat();
}

void IoUringGateway::request_service_health(uint32_t request_id) {
    static std::atomic<uint64_t> sequence{1};
    const uint64_t nonce = (static_cast<uint64_t>(request_id) << 32) |
                           sequence.fetch_add(1, std::memory_order_relaxed);
    router_.health_request(nonce);
}

void IoUringGateway::subscribe(const std::string& topic) {
    router_.subscribe(topic);
}

void IoUringGateway::publish(const std::string& topic, const uint8_t* data, size_t len) {
    router_.publish(topic, data, len);
}

// =========================================================================
//  Session announcements
// =========================================================================

void IoUringGateway::announce_session_join(
    const std::shared_ptr<gtnh::net::ClientSession>& session) {
    flatbuffers::FlatBufferBuilder fbb;
    auto name = fbb.CreateString(session->nickname);
    auto joined = Protocol::CreatePlayerJoined(fbb, session->player_id, name);
    fbb.Finish(joined);
    publish("player.joined", fbb.GetBufferPointer(), fbb.GetSize());
    request_spawn_chunk(session->session_id);
}

void IoUringGateway::announce_session_left(
    const std::shared_ptr<gtnh::net::ClientSession>& session) {
    flatbuffers::FlatBufferBuilder fbb;
    auto left = Protocol::CreatePlayerLeft(
        fbb, session->player_id, session->last_x, session->last_y, session->last_z);
    fbb.Finish(left);
    publish("player.left", fbb.GetBufferPointer(), fbb.GetSize());
    spdlog::info("Gateway: player left (id={}, pos=[{}, {}, {}])", session->player_id,
                 session->last_x, session->last_y, session->last_z);
}

void IoUringGateway::request_chunk(uint64_t session_id, gtnh::net::ChunkCoord coord) {
    auto session = sessions_.by_id(session_id);
    if (!session) return;
    // The pending entry is what makes delivery per-session: chunkd answers a
    // request by broadcasting world.chunk.loaded.compressed, and only the
    // sessions that asked for this coordinate are served from it.
    sessions_.note_chunk_request(session_id, coord);

    flatbuffers::FlatBufferBuilder fbb;
    auto pos = Protocol::Vec3i(coord.x, coord.y, coord.z);
    auto action = Protocol::CreatePlayerAction(
        fbb, session->player_id, Protocol::PlayerActionType_CHUNK_REQUEST, &pos);
    fbb.Finish(action);
    spdlog::info("Gateway: session {} requests chunk ({},{},{})", session_id,
                 coord.x, coord.y, coord.z);
    publish("chunk.requests", fbb.GetBufferPointer(), fbb.GetSize());
}

void IoUringGateway::request_spawn_chunk(uint64_t session_id) {
    auto session = sessions_.by_id(session_id);
    if (!session) return;
    request_chunk(session_id, {session->last_x >> 5, session->last_y >> 5,
                               session->last_z >> 5});
}

void IoUringGateway::retire(std::shared_ptr<gtnh::net::IoUringConnection> conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(retired_mutex_);
    retired_.push_back(std::move(conn));
}

void IoUringGateway::drain_retired() {
    std::vector<std::shared_ptr<gtnh::net::IoUringConnection>> dead;
    {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        dead.swap(retired_);
    }
    // Destroyed here, on the main loop's thread: a closing connection cannot be
    // destroyed on its own poll thread (~IoUringConnection cannot join itself,
    // and a still-joinable std::thread terminates the process).
    dead.clear();
}

std::shared_ptr<gtnh::net::IoUringConnection>
IoUringGateway::take_pending_bulk(uint64_t token) {
    std::lock_guard<std::mutex> lock(pending_bulk_mutex_);
    auto it = pending_bulk_.find(token);
    if (it == pending_bulk_.end()) return nullptr;
    auto conn = std::move(it->second);
    pending_bulk_.erase(it);
    return conn;
}

void IoUringGateway::publish_player_joined() {
    for (const auto &session : sessions_.snapshot()) {
        flatbuffers::FlatBufferBuilder fbb;
        auto name = fbb.CreateString(session->nickname);
        auto joined = Protocol::CreatePlayerJoined(fbb, session->player_id, name);
        fbb.Finish(joined);
        publish("player.joined", fbb.GetBufferPointer(), fbb.GetSize());
        spdlog::info("Gateway: republished player.joined (id={}) after router reconnect",
                     session->player_id);
    }
}

void IoUringGateway::publish_player_left() {
    for (const auto &session : sessions_.snapshot()) {
        flatbuffers::FlatBufferBuilder fbb;
        auto left = Protocol::CreatePlayerLeft(
            fbb, session->player_id, session->last_x, session->last_y, session->last_z);
        fbb.Finish(left);
        publish("player.left", fbb.GetBufferPointer(), fbb.GetSize());
        spdlog::info("Gateway: publish player.left (id={}, pos=[{}, {}, {}])",
                     session->player_id, session->last_x, session->last_y, session->last_z);
    }
}

// =========================================================================
//  Client I/O — targeted sends
// =========================================================================

std::vector<SessionSocket>
IoUringGateway::collect_ctrl_sockets(uint64_t exclude_player_id) const {
    std::shared_lock<std::shared_mutex> guard(conn_mutex_);
    std::vector<SessionSocket> sockets;
    for (const auto &session : sessions_.snapshot()) {
        if (session->player_id == exclude_player_id) continue;
        if (!session->has_ctrl()) continue;
        sockets.push_back({session->player_id, session->ctrl});
    }
    return sockets;
}

std::vector<SessionSocket>
IoUringGateway::collect_bulk_sockets(uint64_t exclude_player_id) const {
    std::shared_lock<std::shared_mutex> guard(conn_mutex_);
    std::vector<SessionSocket> sockets;
    for (const auto &session : sessions_.snapshot()) {
        if (session->player_id == exclude_player_id) continue;
        if (!session->has_bulk()) continue;
        sockets.push_back({session->player_id, session->bulk});
    }
    return sockets;
}

std::shared_ptr<gtnh::net::IoUringConnection>
IoUringGateway::session_ctrl_socket(uint64_t session_id) const {
    std::shared_lock<std::shared_mutex> guard(conn_mutex_);
    auto session = sessions_.by_id(session_id);
    if (!session || !session->has_ctrl()) return nullptr;
    return session->ctrl;
}

std::shared_ptr<gtnh::net::IoUringConnection>
IoUringGateway::session_bulk_socket(uint64_t session_id) const {
    std::shared_lock<std::shared_mutex> guard(conn_mutex_);
    auto session = sessions_.by_id(session_id);
    if (!session || !session->has_bulk()) return nullptr;
    return session->bulk;
}

void IoUringGateway::send_to_session_ctrl(uint64_t session_id, uint8_t msg_type,
                                          const uint8_t* data, size_t len) {
    if (auto conn = session_ctrl_socket(session_id)) conn->send(msg_type, data, len);
}

bool IoUringGateway::send_to_session_bulk(uint64_t session_id, uint8_t msg_type,
                                          const uint8_t* data, size_t len) {
    auto conn = session_bulk_socket(session_id);
    if (!conn) {
        spdlog::debug("Gateway: session {} has no bulk socket, dropping type={}",
                      session_id, msg_type);
        return false;
    }
    conn->send(msg_type, data, len);
    return true;
}

void IoUringGateway::send_to_player_ctrl(uint64_t player_id, uint8_t msg_type,
                                         const uint8_t* data, size_t len) {
    std::shared_lock<std::shared_mutex> guard(conn_mutex_);
    auto session = sessions_.by_player(player_id);
    if (!session || !session->has_ctrl()) {
        spdlog::debug("Gateway: no session for player {} (type={})", player_id, msg_type);
        return;
    }
    session->ctrl->send(msg_type, data, len);
}

// =========================================================================
//  Client I/O — fanout
//
//  Every fanout copies the connections under conn_mutex_ and writes outside
//  it: the copy keeps a socket alive for the write even if the session is torn
//  down meanwhile, and the copy itself cannot race a concurrent detach.
// =========================================================================

void IoUringGateway::broadcast_ctrl(uint8_t msg_type, const uint8_t* data,
                                    size_t len, uint64_t exclude_player_id) {
    for (const auto &socket : collect_ctrl_sockets(exclude_player_id))
        socket.conn->send(msg_type, data, len);
}

void IoUringGateway::broadcast_ctrl_raw(std::shared_ptr<std::vector<uint8_t>> frame,
                                        uint64_t exclude_player_id) {
    for (const auto &socket : collect_ctrl_sockets(exclude_player_id))
        socket.conn->send_raw(frame);
}

void IoUringGateway::broadcast_bulk(uint8_t msg_type, const uint8_t* data,
                                    size_t len, uint64_t exclude_player_id) {
    for (const auto &socket : collect_bulk_sockets(exclude_player_id))
        socket.conn->send(msg_type, data, len);
}

size_t IoUringGateway::session_count() const { return sessions_.size(); }

// =========================================================================
//  Shutdown
// =========================================================================

void IoUringGateway::shutdown() {
    spdlog::info("Gateway: shutting down...");
    // Flush player positions before disconnecting from router — after
    // router_.disconnect() the publish would silently fail and the last
    // positions would be lost.
    publish_player_left();
    router_.disconnect();
    // Retired, not destroyed in place: close() fires on_closed on each
    // connection's own poll thread, and the objects themselves must die on
    // this thread.
    std::vector<SessionSocket> sockets;
    {
        std::unique_lock<std::shared_mutex> guard(conn_mutex_);
        for (const auto &session : sessions_.snapshot()) {
            if (session->has_ctrl()) sockets.push_back({session->player_id, session->ctrl});
            if (session->has_bulk()) sockets.push_back({session->player_id, session->bulk});
        }
    }
    // close() outside the lock: it drains in-flight writes, which can block,
    // and no other session's send may wait on that.
    for (const auto &socket : sockets) {
        retire(socket.conn);
        socket.conn->close();
    }
    {
        std::lock_guard<std::mutex> lock(pending_bulk_mutex_);
        for (auto &[token, conn] : pending_bulk_) {
            retire(conn);
            conn->close();
        }
        pending_bulk_.clear();
    }
    ctrl_server_.stop();
    bulk_server_.stop();
    // The retired connections are joined here; their poll threads have had the
    // whole stop sequence to notice the closed sockets.
    drain_retired();
    spdlog::info("Gateway: stopped");
}

// =========================================================================
//  Client intent admission
// =========================================================================

bool IoUringGateway::admit_and_publish(uint64_t session_id, uint64_t claimed_player_id,
                                       const char *what, const std::string &topic,
                                       const uint8_t* data, size_t len) {
    auto session = sessions_.by_id(session_id);
    if (!session) {
        spdlog::warn("Gateway: {} from unknown session {} dropped", what, session_id);
        return false;
    }
    // One admission rule for every client→server frame that names a player:
    // the identity must be the one this session was given.
    const auto verdict = gtnh::net::admit_intent(*session, claimed_player_id);
    if (verdict != gtnh::net::IntentVerdict::Accept) {
        spdlog::warn("Gateway: {} rejected ({}): session {} is player {}, frame claims {}",
                     what, gtnh::net::to_string(verdict), session_id, session->player_id,
                     claimed_player_id);
        return false;
    }
    publish(topic, data, len);
    return true;
}

// =========================================================================
//  Client read callbacks
// =========================================================================

void IoUringGateway::on_client_bulk_message(const std::shared_ptr<ClientConnContext>& ctx,
                                            uint8_t msg_type, const uint8_t* data,
                                            size_t len) {
    if (msg_type != GatewayMsg::kSessionHello) {
        spdlog::warn("Gateway: unexpected msg_type={} on bulk socket ({} bytes)",
                     msg_type, len);
        return;
    }
    if (ctx->session_id.load(std::memory_order_acquire) != 0) {
        spdlog::warn("Gateway: bulk socket sent a second SessionHello, ignored");
        return;
    }
    auto conn = take_pending_bulk(ctx->bulk_token);
    if (!conn) {
        spdlog::error("Gateway: bulk connection is no longer parked, dropping");
        return;
    }
    flatbuffers::Verifier v(data, len);
    if (!v.VerifyBuffer<Protocol::SessionHello>(nullptr)) {
        spdlog::error("Gateway: invalid SessionHello on bulk socket");
        conn->close();
        retire(std::move(conn));
        return;
    }
    const uint64_t session_id =
        flatbuffers::GetRoot<Protocol::SessionHello>(data)->session_id();
    bool attached;
    {
        std::unique_lock<std::shared_mutex> guard(conn_mutex_);
        attached = sessions_.attach_bulk(session_id, conn);
    }
    if (!attached) {
        spdlog::warn("Gateway: bulk socket named unknown session {}, closing", session_id);
        conn->close();
        retire(std::move(conn));
        return;
    }
    ctx->session_id.store(session_id, std::memory_order_release);
    auto session = sessions_.by_id(session_id);
    spdlog::info("Gateway: bulk socket paired to session {} (player {})", session_id,
                 session ? session->player_id : 0);

    // The join request for this session's chunk was published before any bulk
    // socket existed, and ChunkStore answers exactly once: without this
    // re-request the session would wait forever for a chunk already answered
    // to nobody.
    for (const auto &coord : sessions_.pending_chunks(session_id))
        request_chunk(session_id, coord);
}

void IoUringGateway::on_client_ctrl_message(uint64_t session_id, uint8_t msg_type,
                                            const uint8_t* data, size_t len) {
    switch (msg_type) {
    case GatewayMsg::kPlayerAction: {
        flatbuffers::Verifier v(data, len);
        auto action = v.VerifyBuffer<Protocol::PlayerAction>(nullptr)
            ? flatbuffers::GetRoot<Protocol::PlayerAction>(data) : nullptr;
        if (!action) { spdlog::error("Gateway: invalid PlayerAction on ctrl"); return; }
        if (!admit_and_publish(session_id, action->player_id(), "PlayerAction",
                               "player.actions", data, len))
            return;
        auto session = sessions_.by_id(session_id);
        if (action->pos() && session) {
            session->last_x = action->pos()->x();
            session->last_y = action->pos()->y();
            session->last_z = action->pos()->z();
        }
        switch (action->action()) {
        case Protocol::PlayerActionType_CHUNK_REQUEST: {
            if (!action->pos() || !session) return;
            // Request-scoped delivery: remember who asked, and attribute the
            // upstream request to this session's player, not to whatever id
            // the client typed.
            const gtnh::net::ChunkCoord coord{action->pos()->x(), action->pos()->y(),
                                              action->pos()->z()};
            sessions_.note_chunk_request(session_id, coord);
            flatbuffers::FlatBufferBuilder fbb;
            auto pos = Protocol::Vec3i(coord.x, coord.y, coord.z);
            auto attributed = Protocol::CreatePlayerAction(
                fbb, session->player_id, Protocol::PlayerActionType_CHUNK_REQUEST, &pos);
            fbb.Finish(attributed);
            publish("chunk.requests", fbb.GetBufferPointer(), fbb.GetSize());
            return;
        }
        case Protocol::PlayerActionType_ITEM_ACTION:
            // admit_and_publish above already forwarded this item action.
            return;
        default:
            // MOVE/UNLOAD are dropped as flood control: the client sends them
            // at ~15k/s while walking, and they saturate the router. Position
            // streaming is tracked as gp-rxry.
            spdlog::debug("Gateway: dropped PlayerAction action={}", (int)action->action());
            return;
        }
    }
    case GatewayMsg::kSetBlockAction: {
        flatbuffers::Verifier v(data, len);
        auto action = v.VerifyBuffer<Protocol::SetBlockAction>(nullptr)
            ? flatbuffers::GetRoot<Protocol::SetBlockAction>(data) : nullptr;
        if (!action) { spdlog::error("Gateway: invalid SetBlockAction on ctrl"); return; }
        // Correlate the ACK: BlockAck carries only request_id, so the session
        // that asked is remembered here instead of broadcasting the reply.
        sessions_.remember_request(session_id, action->request_id());
        if (!admit_and_publish(session_id, action->player_id(), "SetBlockAction",
                               "player.actions.setblock", data, len))
            return;
        auto session = sessions_.by_id(session_id);
        if (action->pos() && session) {
            session->last_x = action->pos()->x();
            session->last_y = action->pos()->y();
            session->last_z = action->pos()->z();
        }
        return;
    }
    case GatewayMsg::kInventoryAction: {
        flatbuffers::Verifier v(data, len);
        auto* act = v.VerifyBuffer<Protocol::InventoryAction>(nullptr)
            ? flatbuffers::GetRoot<Protocol::InventoryAction>(data) : nullptr;
        if (!act) { spdlog::error("Gateway: invalid InventoryAction on ctrl"); return; }
        spdlog::debug("[Gateway] InventoryAction pid={} act={} cid={} slot={}",
                      act->player_id(), act->action_type(), act->container_id(), act->slot());
        admit_and_publish(session_id, act->player_id(), "InventoryAction",
                          "player.inventory.actions", data, len);
        break;
    }
    case GatewayMsg::kCraftRequest: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::CraftRequest>(nullptr)
            ? flatbuffers::GetRoot<Protocol::CraftRequest>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid CraftRequest on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "CraftRequest",
                          "sim.craft.request", data, len);
        break;
    }
    case GatewayMsg::kSetMachineSlot: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::SetMachineSlotReq>(nullptr)
            ? flatbuffers::GetRoot<Protocol::SetMachineSlotReq>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid SetMachineSlotReq on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "SetMachineSlot",
                          "player.machine.slot", data, len);
        break;
    }
    case GatewayMsg::kToolAction: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::ToolAction>(nullptr)
            ? flatbuffers::GetRoot<Protocol::ToolAction>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid ToolAction on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "ToolAction",
                          "player.tool.action", data, len);
        break;
    }
    case GatewayMsg::kQuestCompleteRequest: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::QuestCompleteRequest>(nullptr)
            ? flatbuffers::GetRoot<Protocol::QuestCompleteRequest>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid QuestCompleteRequest on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "QuestCompleteRequest",
                          "quest.complete.request", data, len);
        break;
    }
    case GatewayMsg::kQuestBookOpen: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::QuestBookOpen>(nullptr)
            ? flatbuffers::GetRoot<Protocol::QuestBookOpen>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid QuestBookOpen on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "QuestBookOpen",
                          "quest.book.open", data, len);
        break;
    }
    case GatewayMsg::kQuestExchangeRequest: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::QuestExchangeRequest>(nullptr)
            ? flatbuffers::GetRoot<Protocol::QuestExchangeRequest>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid QuestExchangeRequest on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "QuestExchangeRequest",
                          "quest.exchange.request", data, len);
        break;
    }
    case GatewayMsg::kQuestExchangeCooldownGet: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::QuestExchangeCooldownGet>(nullptr)
            ? flatbuffers::GetRoot<Protocol::QuestExchangeCooldownGet>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid QuestExchangeCooldownGet on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "QuestExchangeCooldownGet",
                          "quest.exchange.cooldown.get", data, len);
        break;
    }
    case GatewayMsg::kGameModeChange: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::GameModeChange>(nullptr)
            ? flatbuffers::GetRoot<Protocol::GameModeChange>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid GameModeChange on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "GameModeChange",
                          "player.gamemode.change", data, len);
        break;
    }
    case GatewayMsg::kStartScenarioReq: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::StartScenarioReq>(nullptr)
            ? flatbuffers::GetRoot<Protocol::StartScenarioReq>(data) : nullptr;
        if (!req) { spdlog::error("Gateway: invalid StartScenarioReq on ctrl"); return; }
        admit_and_publish(session_id, req->player_id(), "StartScenarioReq",
                          "player.scenario.start", data, len);
        break;
    }
    // Container windows (machine/chest/workbench open+close): one payload type,
    // ContainerOpenReq{player_id, pos}, routed to four topics.
    case GatewayMsg::kMachineOpenReq:
    case GatewayMsg::kChestOpenReq:
    case GatewayMsg::kChestCloseReq:
    case GatewayMsg::kMachineCloseReq:
    case GatewayMsg::kWorkbenchOpenReq: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)
            ? flatbuffers::GetRoot<Protocol::ContainerOpenReq>(data) : nullptr;
        if (!req) {
            spdlog::error("Gateway: invalid ContainerOpenReq on ctrl (type={})", msg_type);
            return;
        }
        const char* topic = nullptr;
        switch (msg_type) {
        case GatewayMsg::kMachineOpenReq:    topic = "player.machine.open"; break;
        case GatewayMsg::kChestOpenReq:      topic = "player.chest.open"; break;
        case GatewayMsg::kChestCloseReq:     topic = "player.chest.close"; break;
        case GatewayMsg::kMachineCloseReq:   topic = "player.machine.close"; break;
        case GatewayMsg::kWorkbenchOpenReq:  topic = "sim.workbench.load"; break;
        }
        admit_and_publish(session_id, req->player_id(), "ContainerOpenReq", topic, data, len);
        break;
    }
    // Server-driven recipe queries → RecipeManagerService (RecipeFrame payload).
    // The gateway is a pure pass-through: it only verifies the frame and
    // forwards to the recipe.* topic; replies come back on recipe.*.response.
    case GatewayMsg::kRecipeCheckReq:
    case GatewayMsg::kRecipeCatalogReq:
    case GatewayMsg::kRecipeItemReq:
    case GatewayMsg::kRecipeMachineReq: {
        flatbuffers::Verifier v(data, len);
        if (!Protocol::VerifyRecipeFrameBuffer(v)) {
            spdlog::error("Gateway: invalid RecipeFrame on ctrl msg_type {}", msg_type); return;
        }
        const char* topic = nullptr;
        switch (msg_type) {
            case GatewayMsg::kRecipeCheckReq:   topic = "recipe.check"; break;
            case GatewayMsg::kRecipeCatalogReq: topic = "recipe.catalog"; break;
            case GatewayMsg::kRecipeItemReq:    topic = "recipe.item"; break;
            case GatewayMsg::kRecipeMachineReq: topic = "recipe.machine"; break;
        }
        publish(topic, data, len);
        break;
    }
    // Debug pipe-contents query → PipeNetworkService. Pure pass-through:
    // verify the PipeContentsReq frame, forward to pipe.contents.request, then
    // relay the PipeContentsResp back on pipe.contents.response → ctrl.
    case GatewayMsg::kPipeContentsReq: {
        flatbuffers::Verifier v(data, len);
        auto* req = v.VerifyBuffer<Protocol::PipeContentsReq>(nullptr)
            ? flatbuffers::GetRoot<Protocol::PipeContentsReq>(data) : nullptr;
        if (!req) {
            spdlog::error("Gateway: invalid PipeContentsReq on ctrl"); return;
        }
        admit_and_publish(session_id, req->player_id(), "PipeContentsReq",
                          "pipe.contents.request", data, len);
        break;
    }
    case GatewayMsg::kServiceHealthReq: {
        flatbuffers::Verifier v(data, len);
        auto *req = v.VerifyBuffer<Protocol::ServiceHealthReq>(nullptr)
            ? flatbuffers::GetRoot<Protocol::ServiceHealthReq>(data) : nullptr;
        if (!req) {
            spdlog::error("Gateway: invalid ServiceHealthReq on ctrl"); return;
        }
        spdlog::info("Gateway: received ServiceHealthReq request_id={}", req->request_id());
        request_service_health(req->request_id());
        break;
    }
    default: spdlog::warn("Gateway: unknown ctrl client msg type {}", msg_type); break;
    }
}
