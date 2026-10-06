#include "test.h"

#include <gtnh/net/client_session.h>

#include <cstdio>

using gtnh::net::ChunkCoord;
using gtnh::net::ClientSession;
using gtnh::net::IntentVerdict;
using gtnh::net::SessionRegistry;

// These cases pin the two invariants the gateway's multiplayer behaviour rests
// on: a session may act only under the identity it was assigned, and delivery
// state (chunks, request correlation) never leaks between sessions.

// A socket is never needed for identity bookkeeping, and constructing one
// requires a live fd, so registry cases pass nullptr where they only exercise
// identity.
static std::shared_ptr<ClientSession> add(SessionRegistry& registry) {
  return registry.create_session(nullptr);
}

static void test_identity_is_unique_per_session() {
  SessionRegistry registry;
  auto a = add(registry);
  auto b = add(registry);
  CHECK_NE(a->session_id, b->session_id);
  CHECK_NE(a->player_id, b->player_id);
  CHECK_EQ(registry.size(), 2u);
}

static void test_requested_id_is_honoured_when_free() {
  SessionRegistry registry;
  auto a = registry.create_session(nullptr, 7);
  CHECK_EQ(a->player_id, 7u);
  // A second client asking for the same id is renumbered, not merged: the
  // gateway would otherwise be routing two clients as one player.
  auto b = registry.create_session(nullptr, 7);
  CHECK_EQ(b->player_id, 8u);
  CHECK(registry.by_player(7) != nullptr);
  CHECK_EQ(registry.by_player(7)->session_id, a->session_id);
}

static void test_removed_id_is_reusable_after_teardown() {
  SessionRegistry registry;
  auto a = registry.create_session(nullptr, 5);
  auto removed = registry.remove_session(a->session_id);
  CHECK(removed != nullptr);
  CHECK_EQ(removed->player_id, 5u);
  CHECK(registry.by_player(5) == nullptr);
  auto b = registry.create_session(nullptr, 5);
  CHECK_EQ(b->player_id, 5u);
}

static void test_spoofed_identity_is_rejected() {
  SessionRegistry registry;
  auto session = add(registry);
  CHECK(admit_intent(*session, session->player_id) == IntentVerdict::Accept);
  // 0 means "unclaimed" and is attributed to the session server-side.
  CHECK(admit_intent(*session, 0) == IntentVerdict::Accept);
  // Another player's id is a spoof, even though that player exists.
  auto other = add(registry);
  CHECK(admit_intent(*session, other->player_id) ==
        IntentVerdict::RejectSpoofedId);
}

static void test_snapshot_excludes_one_player() {
  SessionRegistry registry;
  auto a = add(registry);
  add(registry);
  auto others = registry.snapshot_except(a->player_id);
  CHECK_EQ(others.size(), 1u);
  CHECK_NE(others[0]->player_id, a->player_id);
}

static void test_chunk_delivery_is_request_scoped() {
  SessionRegistry registry;
  auto requester = add(registry);
  auto bystander = add(registry);

  const ChunkCoord coord{37, 1, 37};
  registry.note_chunk_request(requester->session_id, coord);

  auto subscribers = registry.peek_chunk_subscribers(coord);
  CHECK_EQ(subscribers.size(), 1u);
  CHECK_EQ(subscribers[0]->session_id, requester->session_id);
  CHECK(bystander->session_id != requester->session_id);

  registry.consume_chunk_request(requester->session_id, coord);
  // Consumed on delivery: a second arrival of the same chunk finds nobody, so a
  // stale request cannot pin a session to a region.
  CHECK_EQ(registry.peek_chunk_subscribers(coord).size(), 0u);
}

static void test_pending_entry_survives_a_skipped_delivery() {
  SessionRegistry registry;
  auto requester = add(registry);
  const ChunkCoord coord{1, 2, 3};
  registry.note_chunk_request(requester->session_id, coord);

  // Peeking must not consume: a session with no delivery socket yet still has
  // to be served once it pairs one, because ChunkStore answers a chunk request
  // exactly once.
  CHECK_EQ(registry.peek_chunk_subscribers(coord).size(), 1u);
  CHECK_EQ(registry.pending_chunks(requester->session_id).size(), 1u);
  registry.consume_chunk_request(requester->session_id, coord);
  CHECK_EQ(registry.pending_chunks(requester->session_id).size(), 0u);
}

static void test_unrequested_chunk_reaches_nobody() {
  SessionRegistry registry;
  add(registry);
  CHECK_EQ(registry.peek_chunk_subscribers({1, 1, 1}).size(), 0u);
}

static void test_request_correlation_is_per_session() {
  SessionRegistry registry;
  auto a = add(registry);
  auto b = add(registry);

  // Same request id from two sessions: correlation must not cross over.
  registry.remember_request(a->session_id, 4242);
  registry.remember_request(b->session_id, 4242);
  auto owners = registry.claim_request(4242);
  CHECK_EQ(owners.size(), 2u);

  // Correlation is consumed, so a late duplicate reply is not re-delivered.
  CHECK_EQ(registry.claim_request(4242).size(), 0u);
}

static void test_request_zero_is_not_correlated() {
  SessionRegistry registry;
  auto a = add(registry);
  registry.remember_request(a->session_id, 0);
  CHECK_EQ(registry.claim_request(0).size(), 0u);
}

static void test_session_teardown_drops_its_correlations() {
  SessionRegistry registry;
  auto a = add(registry);
  registry.remember_request(a->session_id, 77);
  auto removed = registry.remove_session(a->session_id);
  CHECK(removed != nullptr);
  // The mapping must not outlive the session that created it, or a later
  // client reusing the id would receive somebody else's reply.
  CHECK_EQ(registry.claim_request(77).size(), 0u);
}

static void test_interest_window_predicate() {
  gtnh::net::PlayerInterest interest;
  interest.center_x = 8;
  interest.center_y = 2;
  interest.center_z = 7;
  interest.radius = 2;
  CHECK(interest.ShouldSendChunk({9, 2, 7}));
  CHECK(!interest.ShouldSendChunk({40, 2, 7}));
}

void test_session_registry() {
  printf("  - identity_is_unique_per_session\n"); test_identity_is_unique_per_session();
  printf("  - requested_id_is_honoured_when_free\n"); test_requested_id_is_honoured_when_free();
  printf("  - removed_id_is_reusable_after_teardown\n"); test_removed_id_is_reusable_after_teardown();
  printf("  - spoofed_identity_is_rejected\n"); test_spoofed_identity_is_rejected();
  printf("  - snapshot_excludes_one_player\n"); test_snapshot_excludes_one_player();
  printf("  - chunk_delivery_is_request_scoped\n"); test_chunk_delivery_is_request_scoped();
  printf("  - pending_entry_survives_a_skipped_delivery\n"); test_pending_entry_survives_a_skipped_delivery();
  printf("  - unrequested_chunk_reaches_nobody\n"); test_unrequested_chunk_reaches_nobody();
  printf("  - request_correlation_is_per_session\n"); test_request_correlation_is_per_session();
  printf("  - request_zero_is_not_correlated\n"); test_request_zero_is_not_correlated();
  printf("  - session_teardown_drops_its_correlations\n"); test_session_teardown_drops_its_correlations();
  printf("  - interest_window_predicate\n"); test_interest_window_predicate();
  printf("  [SESSION_REGISTRY PASS]\n");
}
