// gateway.h
#pragma once

#include <common/GatewayMsg.h>

#include <gtnh/net/client_session.h>
#include <gtnh/net/io_uring_connection.h>
#include <gtnh/net/router_client.h>
#include <gtnh/net/server.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Gateway ↔ Client wire protocol
//
// Frame: [4 bytes: payload size BE] [1 byte: message type] [FlatBuffer data]
//
// Message type constants live in <common/GatewayMsg.h> — the single source of
// truth shared with the game client (NetClient.h).
//
// Session handshake (see src/engine/net/include/gtnh/net/client_session.h):
//   52 = SessionWelcome (gateway → ctrl) — server-assigned player_id
//   53 = SessionHello   (client → bulk) — binds the bulk socket to a session
//
// Types (client → gateway):
//   1 = PlayerAction (FlatBuffer: Protocol::PlayerAction)
//   7 = InventoryAction (FlatBuffer: Protocol::InventoryAction)
//  11 = SetBlockAction (FlatBuffer: Protocol::SetBlockAction)
//  13 = ToolAction (FlatBuffer: Protocol::ToolAction)
//
// Types (gateway → client):
//   2 = ChunkData (FlatBuffer: Protocol::ChunkData)
//   3 = EntitySnapshot (FlatBuffer: Protocol::EntitySnapshot)
//   6 = InventoryUpdate (FlatBuffer: Protocol::InventoryUpdate)
//  14 = ToolActionResp (FlatBuffer: Protocol::ToolActionResp)
// ---------------------------------------------------------------------------

// Top-level Gateway: owns io_uring rings, listens for GameClient connections,
// connects to MessageRouter, relays messages bidirectionally.
//
// Every client connection belongs to a ClientSession (gtnh-net). A session —
// not "the client" — is the unit of routing: ctrl and bulk sockets, the
// server-assigned player_id, the chunks it is waiting for. Sessions are
// independent, so one client disconnecting cannot disturb another.
// Per-connection context for the io_uring callbacks, which carry no
// connection identity of their own. The accept handler parks the session id
// here, and for bulk the token under which its still-unpaired connection is
// parked in the gateway.
//
// It deliberately does NOT hold the connection: a connection owns the
// callbacks that capture this context, so holding it back would be a cycle
// that leaks a poll thread per client.
struct ClientConnContext {
  std::atomic<uint64_t> session_id{0};
  uint64_t bulk_token = 0;
};

// A session's connection, copied out from under the connection lock so the
// caller can write without holding it.
struct SessionSocket {
  uint64_t player_id = 0;
  std::shared_ptr<gtnh::net::IoUringConnection> conn;
};

class IoUringGateway {
public:
  IoUringGateway() = default;
  ~IoUringGateway();

  IoUringGateway(const IoUringGateway &) = delete;
  IoUringGateway &operator=(const IoUringGateway &) = delete;

  bool listen(uint16_t ctrl_port, uint16_t bulk_port);
  bool connect_router(const std::string &host, uint16_t port);
  bool connect_router(); // re-registers with stored topics

  void subscribe(const std::string &topic);
  void publish(const std::string &topic, const uint8_t *data, size_t len);

  void sendHeartbeat();
  void request_service_health(uint32_t request_id);
  bool is_router_connected() const { return router_.is_connected(); }

  // ── Session routing ──────────────────────────────────────────────────
  //
  // Targeted: exactly one session.
  void send_to_session_ctrl(uint64_t session_id, uint8_t msg_type,
                            const uint8_t *data, size_t len);
  bool send_to_session_bulk(uint64_t session_id, uint8_t msg_type,
                            const uint8_t *data, size_t len);
  // Targeted by identity, for replies that carry a player_id but no session.
  void send_to_player_ctrl(uint64_t player_id, uint8_t msg_type,
                           const uint8_t *data, size_t len);

  // Fanout. exclude_player_id = 0 means "every session".
  void broadcast_ctrl(uint8_t msg_type, const uint8_t *data, size_t len,
                      uint64_t exclude_player_id = 0);
  void broadcast_ctrl_raw(std::shared_ptr<std::vector<uint8_t>> frame,
                          uint64_t exclude_player_id = 0);
  void broadcast_bulk(uint8_t msg_type, const uint8_t *data, size_t len,
                      uint64_t exclude_player_id = 0);

  size_t session_count() const;
  bool has_client() const { return session_count() > 0; }

  // Republishes player.joined for every live session (router reconnect) and
  // player.left for every live session (shutdown, so positions are persisted).
  void publish_player_joined();
  void publish_player_left();

  // Reaps connections whose sockets already closed. Called from the main loop:
  // a connection's on_closed runs on its own poll thread and cannot destroy it
  // there, so closing connections are parked until this runs.
  void drain_retired();
  void shutdown();

  std::function<void(const std::string &topic, const uint8_t *data, size_t len)>
      on_router_message;

private:
  void configure_router_callbacks();
  void on_router_publish(const std::string &topic,
                         std::shared_ptr<std::vector<uint8_t>> data);

  void on_client_ctrl_message(uint64_t session_id, uint8_t msg_type,
                              const uint8_t *data, size_t len);
  void on_client_bulk_message(const std::shared_ptr<ClientConnContext> &ctx,
                              uint8_t msg_type, const uint8_t *data,
                              size_t len);

  // Forwards a verified client frame upstream, after the transport-layer
  // admission check (gtnh::net::admit_intent). Every ctrl message that claims
  // a player_id goes through here.
  bool admit_and_publish(uint64_t session_id, uint64_t claimed_player_id,
                         const char *what, const std::string &topic,
                         const uint8_t *data, size_t len);

  void announce_session_join(const std::shared_ptr<gtnh::net::ClientSession> &session);
  void announce_session_left(const std::shared_ptr<gtnh::net::ClientSession> &session);
  // Requests one chunk on behalf of a session, attributed to that session's
  // player_id and recorded so the answer is delivered to it.
  void request_chunk(uint64_t session_id, gtnh::net::ChunkCoord coord);
  // Requests the chunk the session is standing in, after join/restore.
  void request_spawn_chunk(uint64_t session_id);

  // Collects the connections to write to, copied under conn_mutex_ so a
  // concurrent detach cannot pull the socket out from under a sender.
  std::vector<SessionSocket> collect_ctrl_sockets(uint64_t exclude_player_id = 0) const;
  std::vector<SessionSocket> collect_bulk_sockets(uint64_t exclude_player_id = 0) const;
  std::shared_ptr<gtnh::net::IoUringConnection>
  session_ctrl_socket(uint64_t session_id) const;
  std::shared_ptr<gtnh::net::IoUringConnection>
  session_bulk_socket(uint64_t session_id) const;

  // ── Connection lifetime ──────────────────────────────────────────────
  //
  // A connection's on_closed fires ON that connection's own poll thread.
  // Releasing the last reference to the connection there would run
  // ~IoUringConnection on its own thread, and its destructor cannot join a
  // thread from itself — ~thread() on a still-joinable thread terminates the
  // process. So a closing connection is parked in retired_ and destroyed by the
  // main loop instead (drain_retired).
  void retire(std::shared_ptr<gtnh::net::IoUringConnection> conn);

  // conn_mutex_ guards the connection slots of every live session: a router
  // callback fans out on the router's poll thread while a session's own poll
  // thread may be detaching its socket, and moving a shared_ptr out of a
  // session in one thread while another reads the same member is a data race
  // on the pointer itself (observed as SIGSEGV inside poll()).
  mutable std::shared_mutex conn_mutex_;

  // Takes the connection parked under a bulk token (and forgets the token).
  std::shared_ptr<gtnh::net::IoUringConnection> take_pending_bulk(uint64_t token);

  mutable std::mutex retired_mutex_;
  std::vector<std::shared_ptr<gtnh::net::IoUringConnection>> retired_;

  mutable std::mutex pending_bulk_mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<gtnh::net::IoUringConnection>> pending_bulk_;
  std::atomic<uint64_t> next_bulk_token_{1};

  gtnh::net::TcpServer ctrl_server_;
  gtnh::net::TcpServer bulk_server_;
  gtnh::net::RouterClient router_;

  gtnh::net::SessionRegistry sessions_;
  gtnh::net::TagAllocator tag_alloc_;

  uint16_t ctrl_port_ = 0;
  uint16_t bulk_port_ = 0;

  // Router reconnect state
  std::string router_host_;
  uint16_t router_port_ = 0;
};
