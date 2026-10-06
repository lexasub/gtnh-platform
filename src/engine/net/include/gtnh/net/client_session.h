#pragma once

// ClientSession — the transport-level unit of routing in a multi-client
// gateway.
//
// A session is whatever the server can address: the client's ctrl socket, its
// bulk socket, the canonical identity the server assigned to it, and the
// delivery state the transport itself has to remember (which chunks this
// client is waiting for, which RPC replies belong to it).
//
// Scope rule: this file holds TRANSPORT state only. It knows sockets,
// identities and delivery bookkeeping. It must never learn a game rule —
// what a block placement means, whether an inventory slot is legal, who owns
// a machine. Those live in the gameplay services. The one thing it does own
// is the admission check for inbound client traffic (admit_intent), because
// "which session sent this frame" is a transport question and must be answered
// in exactly one place.

#include <gtnh/net/io_uring_connection.h>
#include <cstdlib>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gtnh::net {

// Chunk coordinates, in chunk units (block >> 5).
struct ChunkCoord {
  int32_t x = 0, y = 0, z = 0;

  bool operator==(const ChunkCoord &other) const {
    return x == other.x && y == other.y && z == other.z;
  }
};

struct ChunkCoordHash {
  size_t operator()(const ChunkCoord &c) const noexcept {
    auto mix = [](int32_t v) {
      return static_cast<size_t>(static_cast<uint32_t>(v)) * 2654435761u;
    };
    return mix(c.x) ^ (mix(c.y) << 1) ^ (mix(c.z) << 2);
  }
};

// The chunk window a session may be served. Filtering by an interest window is
// only meaningful once a session reports a real position; until then delivery
// follows explicit requests (see pending_chunks).
struct PlayerInterest {
  int32_t center_x = 0, center_y = 0, center_z = 0;
  int32_t radius = 8; // chunks

  bool ShouldSendChunk(ChunkCoord cc) const {
    return std::abs(cc.x - center_x) <= radius &&
           std::abs(cc.y - center_y) <= radius &&
           std::abs(cc.z - center_z) <= radius;
  }
};

struct ClientSession {
  uint64_t session_id = 0;
  // Canonical identity assigned by the server. Zero only while a session is
  // still being constructed; every admitted message carries this id.
  uint64_t player_id = 0;
  std::string nickname;

  std::shared_ptr<IoUringConnection> ctrl;
  std::shared_ptr<IoUringConnection> bulk;

  PlayerInterest interest;

  int32_t last_x = 256, last_y = 80, last_z = 224;

  // Chunks this session asked for and has not received yet. Delivery is
  // request-driven: a chunk reaches exactly the sessions that asked for it,
  // and the entry is consumed on delivery so a stale request cannot pin a
  // session to a region it has moved out of.
  std::unordered_set<ChunkCoord, ChunkCoordHash> pending_chunks;

  bool has_bulk() const { return bulk && bulk->is_open(); }
  bool has_ctrl() const { return ctrl && ctrl->is_open(); }
};

// Verdict for an inbound client frame that claims a player_id.
enum class IntentVerdict {
  Accept,
  RejectUnknownSession,
  RejectSpoofedId,
};

// The single admission point for client→server traffic. Every message that
// carries a player_id goes through this before it is published upstream, so
// "who is allowed to act as whom" has exactly one answer in the transport
// layer and cannot drift per call site.
IntentVerdict admit_intent(const ClientSession &session, uint64_t claimed_player_id);

const char *to_string(IntentVerdict verdict);

// Thread-safe registry of live sessions.
//
// Sessions are shared_ptr so a caller can send on a connection after releasing
// the registry lock: fanout must not hold the registry mutex across socket
// writes, otherwise one slow client stalls every other session.
class SessionRegistry {
public:
  // Creates a session for a freshly accepted ctrl connection. The canonical
  // player_id is the requested one when it is free and non-zero, otherwise the
  // lowest free id — a client never gets to pick an id somebody else holds.
  std::shared_ptr<ClientSession> create_session(
      std::shared_ptr<IoUringConnection> ctrl, uint64_t requested_player_id = 0,
      const std::string &nickname = {});

  // Binds a bulk connection to an existing session.
  bool attach_bulk(uint64_t session_id,
                   std::shared_ptr<IoUringConnection> bulk);

  // Removes a session's bulk connection and returns the session, moving that
  // connection into *conn_out. The caller must take ownership: dropping the
  // last reference on the connection's own poll thread would run
  // ~IoUringConnection on that thread, and ~thread() on a still-joinable
  // thread terminates the process. The session itself stays alive: the ctrl
  // socket owns it.
  std::shared_ptr<ClientSession>
  detach_bulk(uint64_t session_id,
              std::shared_ptr<IoUringConnection> *conn_out);

  // Removes a session. Returns the removed state so the caller can publish the
  // corresponding player.left with the last known position.
  std::shared_ptr<ClientSession> remove_session(uint64_t session_id);

  std::shared_ptr<ClientSession> by_id(uint64_t session_id) const;
  std::shared_ptr<ClientSession> by_player(uint64_t player_id) const;

  // Live sessions, safe to use after the registry lock is released.
  std::vector<std::shared_ptr<ClientSession>> snapshot() const;
  std::vector<std::shared_ptr<ClientSession>> snapshot_except(uint64_t player_id) const;
  size_t size() const;

  // Records that a session asked for a chunk.
  void note_chunk_request(uint64_t session_id, ChunkCoord coord);

  // Chunks a session asked for and has not been served yet — used to re-issue
  // those requests once a delivery socket actually exists.
  std::vector<ChunkCoord> pending_chunks(uint64_t session_id) const;

  // Sessions waiting for this chunk, without consuming the pending entries.
  std::vector<std::shared_ptr<ClientSession>>
  peek_chunk_subscribers(ChunkCoord coord) const;

  // Consumes one session's pending entry for a chunk that was actually
  // delivered. Delivery MUST go through this, and only for a session that had
  // a socket to receive it: consuming the entry for a session whose delivery
  // socket does not exist yet would drop the chunk with no way to ask again.
  void consume_chunk_request(uint64_t session_id, ChunkCoord coord);

  // Remembers which session sent a request, so a reply that carries only a
  // request_id can be routed back to its originator. Correlation is
  // per-session: two clients may use the same request_id without crossing.
  void remember_request(uint64_t session_id, uint32_t request_id);

  // Sessions that sent request_id, if any. The mapping is consumed.
  std::vector<std::shared_ptr<ClientSession>> claim_request(uint32_t request_id);

private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<ClientSession>> by_session_id_;
  std::unordered_map<uint64_t, uint64_t> session_id_by_player_;
  std::unordered_map<uint32_t, std::vector<uint64_t>> sessions_by_request_;
  uint64_t next_session_id_ = 1;
  uint64_t next_player_id_ = 1;
};

} // namespace gtnh::net
