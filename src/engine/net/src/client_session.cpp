#include <gtnh/net/client_session.h>
#include <algorithm>
#include <ranges>

namespace gtnh::net {

IntentVerdict admit_intent(const ClientSession &session, uint64_t claimed_player_id) {
  // A claimed id of 0 means "unclaimed": some pre-auth-era frames leave it
  // unset. The session's canonical id is substituted server-side, so this is
  // not spoofing.
  if (claimed_player_id == 0 || claimed_player_id == session.player_id)
    return IntentVerdict::Accept;
  return IntentVerdict::RejectSpoofedId;
}

const char *to_string(IntentVerdict verdict) {
  switch (verdict) {
  case IntentVerdict::Accept: return "accept";
  case IntentVerdict::RejectUnknownSession: return "unknown_session";
  case IntentVerdict::RejectSpoofedId: return "spoofed_id";
  }
  return "unknown";
}

std::shared_ptr<ClientSession>
SessionRegistry::create_session(std::shared_ptr<IoUringConnection> ctrl,
                                 uint64_t requested_player_id,
                                 const std::string &nickname) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto session = std::make_shared<ClientSession>();
  session->session_id = next_session_id_++;
  session->ctrl = std::move(ctrl);

  // Canonical identity: honour the request only when it is actually free.
  // A collision silently renumbers instead of dropping the client — the
  // welcome frame tells the client which id it really has.
  if (requested_player_id != 0 &&
      !session_id_by_player_.contains(requested_player_id)) {
    session->player_id = requested_player_id;
    if (requested_player_id >= next_player_id_)
      next_player_id_ = requested_player_id + 1;
  } else {
    while (session_id_by_player_.contains(next_player_id_))
      ++next_player_id_;
    session->player_id = next_player_id_++;
  }
  session->nickname = nickname.empty() ? ("player-" + std::to_string(session->player_id))
                                       : nickname;

  session_id_by_player_[session->player_id] = session->session_id;
  by_session_id_[session->session_id] = session;
  return session;
}

bool SessionRegistry::attach_bulk(uint64_t session_id,
                                  std::shared_ptr<IoUringConnection> bulk) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = by_session_id_.find(session_id);
  if (it == by_session_id_.end())
    return false;
  it->second->bulk = std::move(bulk);
  return true;
}

std::shared_ptr<ClientSession>
SessionRegistry::detach_bulk(uint64_t session_id,
                             std::shared_ptr<IoUringConnection> *conn_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = by_session_id_.find(session_id);
  if (it == by_session_id_.end())
    return nullptr;
  if (conn_out)
    *conn_out = std::move(it->second->bulk);
  else
    it->second->bulk.reset();
  return it->second;
}

std::shared_ptr<ClientSession>
SessionRegistry::remove_session(uint64_t session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = by_session_id_.find(session_id);
  if (it == by_session_id_.end())
    return nullptr;
  auto session = it->second;
  by_session_id_.erase(it);
  if (session_id_by_player_.contains(session->player_id) &&
      session_id_by_player_[session->player_id] == session_id)
    session_id_by_player_.erase(session->player_id);
  for (auto req_it = sessions_by_request_.begin();
       req_it != sessions_by_request_.end();) {
    auto &owners = req_it->second;
    std::erase(owners, session_id);
    if (owners.empty())
      req_it = sessions_by_request_.erase(req_it);
    else
      ++req_it;
  }
  return session;
}

std::shared_ptr<ClientSession>
SessionRegistry::by_id(uint64_t session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = by_session_id_.find(session_id);
  return it == by_session_id_.end() ? nullptr : it->second;
}

std::shared_ptr<ClientSession>
SessionRegistry::by_player(uint64_t player_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = session_id_by_player_.find(player_id);
  if (it == session_id_by_player_.end())
    return nullptr;
  auto sess = by_session_id_.find(it->second);
  return sess == by_session_id_.end() ? nullptr : sess->second;
}

std::vector<std::shared_ptr<ClientSession>> SessionRegistry::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<ClientSession>> out;
  out.reserve(by_session_id_.size());
  for (const auto &session: by_session_id_ | std::views::values)
    out.push_back(session);
  return out;
}

std::vector<std::shared_ptr<ClientSession>>
SessionRegistry::snapshot_except(uint64_t player_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<ClientSession>> out;
  out.reserve(by_session_id_.size());
  for (const auto &session: by_session_id_ | std::views::values)
    if (session->player_id != player_id)
      out.push_back(session);
  return out;
}

size_t SessionRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return by_session_id_.size();
}

void SessionRegistry::note_chunk_request(uint64_t session_id, ChunkCoord coord) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = by_session_id_.find(session_id);
  if (it == by_session_id_.end())
    return;
  it->second->pending_chunks.insert(coord);
}

std::vector<ChunkCoord>
SessionRegistry::pending_chunks(uint64_t session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ChunkCoord> out;
  auto it = by_session_id_.find(session_id);
  if (it == by_session_id_.end())
    return out;
  out.reserve(it->second->pending_chunks.size());
  for (const auto &coord : it->second->pending_chunks)
    out.push_back(coord);
  return out;
}

std::vector<std::shared_ptr<ClientSession>>
SessionRegistry::peek_chunk_subscribers(ChunkCoord coord) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<ClientSession>> out;
  for (const auto &session: by_session_id_ | std::views::values) {
    if (session->pending_chunks.contains(coord))
      out.push_back(session);
  }
  return out;
}

void SessionRegistry::consume_chunk_request(uint64_t session_id, ChunkCoord coord) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = by_session_id_.find(session_id);
  if (it != by_session_id_.end())
    it->second->pending_chunks.erase(coord);
}

void SessionRegistry::remember_request(uint64_t session_id, uint32_t request_id) {
  if (request_id == 0)
    return; // 0 is "no correlation id" on the wire
  std::lock_guard<std::mutex> lock(mutex_);
  auto &owners = sessions_by_request_[request_id];
  if (std::ranges::find(owners, session_id) == owners.end())
    owners.push_back(session_id);
}

std::vector<std::shared_ptr<ClientSession>>
SessionRegistry::claim_request(uint32_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<ClientSession>> out;
  auto it = sessions_by_request_.find(request_id);
  if (it == sessions_by_request_.end())
    return out;
  for (uint64_t session_id : it->second) {
    auto sess = by_session_id_.find(session_id);
    if (sess != by_session_id_.end())
      out.push_back(sess->second);
  }
  sessions_by_request_.erase(it);
  return out;
}

} // namespace gtnh::net
