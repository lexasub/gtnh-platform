// gateway.cpp
#include "gateway.h"
#include "client_state_generated.h"
#include "core_generated.h"
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

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <shared_mutex>

IoUringGateway::~IoUringGateway() { shutdown(); }

namespace {
constexpr size_t kAccountAuthFrameLimit = 64 * 1024;
constexpr auto kAccountAuthRpcTimeout = std::chrono::seconds(3);
constexpr auto kAccountAuthWindow = std::chrono::minutes(15);
constexpr size_t kAccountFailuresPerName = 5;
constexpr size_t kAccountFailuresPerIp = 20;

struct AccountAuthOutcome {
  Protocol::AccountAuthStatus status = Protocol::AccountAuthStatus_INTERNAL_ERROR;
  std::string account_id;
  uint64_t player_id = 0;
};

std::string rate_limit_username(std::string username) {
  size_t first = 0;
  while (first < username.size() &&
         (username[first] == ' ' || username[first] == '\t'))
    ++first;
  size_t last = username.size();
  while (last > first &&
         (username[last - 1] == ' ' || username[last - 1] == '\t'))
    --last;
  username = username.substr(first, last - first);
  for (char &c : username) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  }
  return username;
}

bool wait_for_fd(int fd, short events,
                 std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return false;
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  pollfd descriptor{fd, events, 0};
  int rc;
  do {
    rc = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
  } while (rc < 0 && errno == EINTR);
  return rc > 0 && (descriptor.revents & events) != 0;
}

bool send_exact(int fd, const uint8_t *data, size_t size,
                std::chrono::steady_clock::time_point deadline) {
  size_t sent = 0;
  while (sent < size) {
    const auto n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        wait_for_fd(fd, POLLOUT, deadline))
      continue;
    return false;
  }
  return true;
}

bool recv_exact(int fd, uint8_t *data, size_t size,
                std::chrono::steady_clock::time_point deadline) {
  size_t received = 0;
  while (received < size) {
    const auto n = ::recv(fd, data + received, size - received, 0);
    if (n > 0) {
      received += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        wait_for_fd(fd, POLLIN, deadline))
      continue;
    return false;
  }
  return true;
}

bool connect_auth_service(int fd, const std::string &address,
                          std::chrono::steady_clock::time_point deadline) {
  const auto colon = address.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == address.size())
    return false;
  const std::string host = address.substr(0, colon);
  const std::string service = address.substr(colon + 1);
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  addrinfo *resolved = nullptr;
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0)
    return false;
  const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(resolved->ai_addr);
  const uint32_t address_be = ntohl(ipv4->sin_addr.s_addr);
  const bool loopback = (address_be >> 24) == 127;
  const bool private_10 = (address_be >> 24) == 10;
  const bool private_172 = (address_be >> 20) == 0xAC1;
  const bool private_192 = (address_be >> 16) == 0xC0A8;
  if (!loopback && !private_10 && !private_172 && !private_192) {
    ::freeaddrinfo(resolved);
    return false;
  }
  const int rc = ::connect(fd, resolved->ai_addr, resolved->ai_addrlen);
  ::freeaddrinfo(resolved);
  if (rc == 0) return true;
  if (errno != EINPROGRESS || !wait_for_fd(fd, POLLOUT, deadline)) return false;
  int socket_error = 0;
  socklen_t error_size = sizeof(socket_error);
  return ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 &&
         socket_error == 0;
}

bool call_account_auth_service(
    const uint8_t *request, size_t request_size, AccountAuthOutcome &outcome,
    std::chrono::steady_clock::time_point accept_deadline) {
  if (!request || request_size == 0 || request_size > kAccountAuthFrameLimit ||
      std::chrono::steady_clock::now() >= accept_deadline)
    return false;
  const char *configured = std::getenv("GTNH_METADB_AUTH_ADDR");
  const std::string address = configured && *configured
                                  ? configured
                                  : "127.0.0.1:5007";
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = std::min(now + kAccountAuthRpcTimeout, accept_deadline);
  if (now >= deadline) return false;
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  struct CloseFd {
    int fd;
    ~CloseFd() { ::close(fd); }
  } close_fd{fd};
  if (!connect_auth_service(fd, address, deadline)) return false;

  const uint32_t frame_size = static_cast<uint32_t>(request_size);
  uint8_t prefix[4] = {static_cast<uint8_t>(frame_size >> 24),
                       static_cast<uint8_t>(frame_size >> 16),
                       static_cast<uint8_t>(frame_size >> 8),
                       static_cast<uint8_t>(frame_size)};
  if (!send_exact(fd, prefix, sizeof(prefix), deadline) ||
      !send_exact(fd, request, request_size, deadline))
    return false;

  uint8_t response_prefix[4]{};
  if (!recv_exact(fd, response_prefix, sizeof(response_prefix), deadline))
    return false;
  const uint32_t response_size =
      (static_cast<uint32_t>(response_prefix[0]) << 24) |
      (static_cast<uint32_t>(response_prefix[1]) << 16) |
      (static_cast<uint32_t>(response_prefix[2]) << 8) |
      static_cast<uint32_t>(response_prefix[3]);
  if (response_size == 0 || response_size > kAccountAuthFrameLimit) return false;
  std::vector<uint8_t> response(response_size);
  if (!recv_exact(fd, response.data(), response.size(), deadline)) return false;

  flatbuffers::Verifier verifier(response.data(), response.size());
  const auto *result = flatbuffers::GetRoot<Protocol::AccountAuthResponse>(response.data());
  if (!result->Verify(verifier)) return false;
  outcome.status = result->status();
  outcome.player_id = result->player_id();
  if (result->account_id()) outcome.account_id = result->account_id()->str();
  return true;
}

} // namespace

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
        ctx->ctrl = conn;
        ctx->auth_token = next_auth_token_.fetch_add(1, std::memory_order_relaxed);
        ctx->auth_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        char peer_ip[INET_ADDRSTRLEN]{};
        if (::getpeername(client_fd, reinterpret_cast<sockaddr *>(&peer), &peer_size) == 0 &&
            ::inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip)))
            ctx->remote_ip = peer_ip;
        else
            ctx->remote_ip = "unknown";

        conn->on_message = [this, ctx](uint8_t type, const uint8_t *d, size_t l) {
            spdlog::debug("Gateway: ctrl frame received (type={}, bytes={})", type, l);
            const uint64_t sid = ctx->session_id.load(std::memory_order_acquire);
            if (sid != 0) {
                on_client_ctrl_message(sid, type, d, l);
                return;
            }
            if (!ctx->authenticated.load(std::memory_order_acquire))
                on_client_auth(ctx, type, d, l);
        };
        conn->on_closed = [this, ctx]() {
            uint64_t sid = ctx->session_id.exchange(0, std::memory_order_acq_rel);
            auto pending_conn = sid == 0 ? ctx->ctrl.lock()
                                         : std::shared_ptr<gtnh::net::IoUringConnection>{};
            std::string account_id;
            {
                std::lock_guard<std::mutex> state(ctx->state_mutex);
                ctx->closed = true;
                account_id = std::move(ctx->account_id);
            }
            {
                std::lock_guard<std::mutex> lock(pending_auth_mutex_);
                pending_auth_.erase(ctx->auth_token);
            }
            std::shared_ptr<gtnh::net::ClientSession> session;
            std::shared_ptr<gtnh::net::IoUringConnection> ctrl_conn;
            std::shared_ptr<gtnh::net::IoUringConnection> bulk_conn;
            {
                std::unique_lock<std::shared_mutex> guard(conn_mutex_);
                if (sid != 0) {
                    session = sessions_.remove_session(sid);
                    if (session) {
                        ctrl_conn = std::move(session->ctrl);
                        bulk_conn = std::move(session->bulk);
                    }
                }
                if (!account_id.empty()) {
                    auto active = active_accounts_.find(account_id);
                    if (active != active_accounts_.end() && active->second == sid)
                        active_accounts_.erase(active);
                }
            }
            if (session) {
                announce_session_left(session);
                retire(std::move(ctrl_conn));
                retire(std::move(bulk_conn));
                spdlog::info("Gateway: ctrl session {} disconnected ({} sessions left)",
                             sid, sessions_.size());
            } else if (pending_conn) {
                retire(std::move(pending_conn));
            }
        };

        bool pending_limit_reached = false;
        {
            std::lock_guard<std::mutex> lock(pending_auth_mutex_);
            pending_limit_reached = pending_auth_.size() >= kMaxPendingAuth;
            if (!pending_limit_reached)
                pending_auth_[ctx->auth_token] = PendingCtrlConnection{conn, ctx};
        }
        if (pending_limit_reached) {
            spdlog::warn("Gateway: rejecting ctrl connection; pending-auth limit reached");
            conn->close();
            return;
        }
        if (!conn->start_reading()) {
            spdlog::error("Gateway: failed to start pending-auth ctrl read loop");
            conn->close();
            return;
        }
        spdlog::info("Gateway: ctrl connection accepted; awaiting account authentication");
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
    reap_pending_auth();
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

bool IoUringGateway::auth_rate_limited(
    const std::string &ip, const std::string &username,
    std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(auth_failure_mutex_);
  auto prune = [now](auto &failures) {
    while (!failures.empty() && now - failures.front() >= kAccountAuthWindow)
      failures.pop_front();
  };
  auto account = account_auth_failures_.find(username);
  if (account != account_auth_failures_.end()) prune(account->second);
  auto remote = ip_auth_failures_.find(ip);
  if (remote != ip_auth_failures_.end()) prune(remote->second);
  return (account != account_auth_failures_.end() &&
          account->second.size() >= kAccountFailuresPerName) ||
         (remote != ip_auth_failures_.end() &&
          remote->second.size() >= kAccountFailuresPerIp);
}

void IoUringGateway::record_auth_failure(
    const std::string &ip, const std::string &username,
    std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(auth_failure_mutex_);
  account_auth_failures_[username].push_back(now);
  ip_auth_failures_[ip].push_back(now);
  auto prune = [now](auto &failures) {
    while (!failures.empty() && now - failures.front() >= kAccountAuthWindow)
      failures.pop_front();
  };
  for (auto it = account_auth_failures_.begin(); it != account_auth_failures_.end();) {
    prune(it->second);
    if (it->second.empty()) it = account_auth_failures_.erase(it);
    else ++it;
  }
  for (auto it = ip_auth_failures_.begin(); it != ip_auth_failures_.end();) {
    prune(it->second);
    if (it->second.empty()) it = ip_auth_failures_.erase(it);
    else ++it;
  }
}

void IoUringGateway::reap_pending_auth() {
  std::vector<std::shared_ptr<gtnh::net::IoUringConnection>> expired;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(pending_auth_mutex_);
    for (auto it = pending_auth_.begin(); it != pending_auth_.end();) {
      auto ctx = it->second.context.lock();
      auto conn = it->second.conn;
      if (!ctx || !conn || ctx->authenticated.load(std::memory_order_acquire)) {
        it = pending_auth_.erase(it);
      } else if (now >= ctx->auth_deadline) {
        expired.push_back(std::move(conn));
        it = pending_auth_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto &conn : expired) conn->close();
}

void IoUringGateway::on_client_auth(
    const std::shared_ptr<ClientConnContext> &ctx, uint8_t msg_type,
    const uint8_t *data, size_t len) {
  auto conn = ctx->ctrl.lock();
  if (!conn) return;
  spdlog::debug("Gateway: received pre-auth ctrl frame (type={}, bytes={})", msg_type, len);
  if (std::chrono::steady_clock::now() >= ctx->auth_deadline ||
      msg_type != GatewayMsg::kAccountAuthRequest || !data || len == 0 ||
      len > kAccountAuthFrameLimit) {
    spdlog::warn("Gateway: closing pre-auth ctrl for invalid message or deadline (type={}, bytes={})",
                 msg_type, len);
    conn->close();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(ctx->state_mutex);
    if (ctx->closed || ctx->auth_inflight) {
      conn->close();
      return;
    }
    ctx->auth_inflight = true;
  }
  struct AttemptReset {
    std::shared_ptr<ClientConnContext> ctx;
    ~AttemptReset() {
      std::lock_guard<std::mutex> lock(ctx->state_mutex);
      ctx->auth_inflight = false;
    }
  } reset{ctx};

  flatbuffers::Verifier verifier(data, len);
  if (!verifier.VerifyBuffer<Protocol::AccountAuthRequest>(nullptr)) {
    spdlog::warn("Gateway: dropping malformed account-auth frame ({} bytes)", len);
    conn->close();
    return;
  }
  const auto *request = flatbuffers::GetRoot<Protocol::AccountAuthRequest>(data);
  if (!request->username() || !request->password()) {
    spdlog::warn("Gateway: account-auth frame is missing required credentials");
    conn->close();
    return;
  }
  const std::string username(request->username()->c_str(), request->username()->size());
  const std::string rate_key = rate_limit_username(username);
  const auto now = std::chrono::steady_clock::now();
  if (auth_rate_limited(ctx->remote_ip, rate_key, now)) {
    flatbuffers::FlatBufferBuilder fbb;
    auto message = fbb.CreateString("Login attempts are temporarily limited.");
    auto result = Protocol::CreateAuthResult(
        fbb, Protocol::AccountAuthStatus_RATE_LIMITED, message);
    fbb.Finish(result);
    conn->send(GatewayMsg::kAuthResult, fbb.GetBufferPointer(), fbb.GetSize());
    return;
  }

  AccountAuthOutcome outcome;
  if (!call_account_auth_service(data, len, outcome, ctx->auth_deadline)) {
    spdlog::warn("Gateway: account-auth RPC to MetaDB failed");
    flatbuffers::FlatBufferBuilder fbb;
    auto message = fbb.CreateString("Authentication service unavailable.");
    auto result = Protocol::CreateAuthResult(
        fbb, Protocol::AccountAuthStatus_INTERNAL_ERROR, message);
    fbb.Finish(result);
    conn->send(GatewayMsg::kAuthResult, fbb.GetBufferPointer(), fbb.GetSize());
    return;
  }
  if (outcome.status != Protocol::AccountAuthStatus_SUCCESS) {
    if (outcome.status == Protocol::AccountAuthStatus_INVALID_CREDENTIALS ||
        outcome.status == Protocol::AccountAuthStatus_USERNAME_TAKEN)
      record_auth_failure(ctx->remote_ip, rate_key, now);
    const char *text = "Authentication failed.";
    if (outcome.status == Protocol::AccountAuthStatus_INVALID_CREDENTIALS)
      text = "Invalid username or password.";
    else if (outcome.status == Protocol::AccountAuthStatus_USERNAME_TAKEN)
      text = "That username is already registered.";
    else if (outcome.status == Protocol::AccountAuthStatus_RATE_LIMITED)
      text = "Login attempts are temporarily limited.";
    else if (outcome.status == Protocol::AccountAuthStatus_INTERNAL_ERROR)
      text = "Authentication service unavailable.";
    flatbuffers::FlatBufferBuilder fbb;
    auto message = fbb.CreateString(text);
    auto result = Protocol::CreateAuthResult(fbb, outcome.status, message);
    fbb.Finish(result);
    conn->send(GatewayMsg::kAuthResult, fbb.GetBufferPointer(), fbb.GetSize());
    return;
  }
  if (outcome.account_id.empty() || outcome.player_id == 0 ||
      !conn->is_open() || std::chrono::steady_clock::now() >= ctx->auth_deadline) {
    conn->close();
    return;
  }

  std::shared_ptr<gtnh::net::ClientSession> session;
  bool account_active = false;
  {
    std::unique_lock<std::shared_mutex> guard(conn_mutex_);
    account_active = active_accounts_.find(outcome.account_id) != active_accounts_.end();
    if (!account_active) {
      session = sessions_.create_session(conn, outcome.player_id, username);
      if (session && session->player_id == outcome.player_id) {
        active_accounts_[outcome.account_id] = session->session_id;
      } else if (session) {
        sessions_.remove_session(session->session_id);
        session.reset();
      }
    }
  }
  if (account_active || !session) {
    const auto status = account_active ? Protocol::AccountAuthStatus_ACCOUNT_ACTIVE
                                       : Protocol::AccountAuthStatus_INTERNAL_ERROR;
    const char *text = account_active ? "This account is already active."
                                      : "Authentication service unavailable.";
    flatbuffers::FlatBufferBuilder fbb;
    auto message = fbb.CreateString(text);
    auto result = Protocol::CreateAuthResult(fbb, status, message);
    fbb.Finish(result);
    conn->send(GatewayMsg::kAuthResult, fbb.GetBufferPointer(), fbb.GetSize());
    return;
  }

  bool canceled = false;
  {
    std::lock_guard<std::mutex> lock(ctx->state_mutex);
    canceled = ctx->closed || std::chrono::steady_clock::now() >= ctx->auth_deadline ||
               !conn->is_open();
    if (!canceled) {
      ctx->account_id = outcome.account_id;
      ctx->authenticated.store(true, std::memory_order_release);
      ctx->session_id.store(session->session_id, std::memory_order_release);
    }
  }
  if (canceled) {
    std::unique_lock<std::shared_mutex> guard(conn_mutex_);
    active_accounts_.erase(outcome.account_id);
    sessions_.remove_session(session->session_id);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(pending_auth_mutex_);
    pending_auth_.erase(ctx->auth_token);
  }

  flatbuffers::FlatBufferBuilder fbb;
  auto message = fbb.CreateString("Authenticated.");
  auto result = Protocol::CreateAuthResult(
      fbb, Protocol::AccountAuthStatus_SUCCESS, message);
  fbb.Finish(result);
  conn->send(GatewayMsg::kAuthResult, fbb.GetBufferPointer(), fbb.GetSize());

  flatbuffers::FlatBufferBuilder welcome_builder;
  auto welcome = Protocol::CreateSessionWelcome(
      welcome_builder, session->session_id, session->player_id);
  welcome_builder.Finish(welcome);
  conn->send(GatewayMsg::kSessionWelcome, welcome_builder.GetBufferPointer(),
             welcome_builder.GetSize());
  spdlog::info("Gateway: authenticated session {} admitted ({} sessions)",
               session->session_id, sessions_.size());
  announce_session_join(session);
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
