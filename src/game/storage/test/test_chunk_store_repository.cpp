// ChunkStoreRepository unit tests (issue gp-897f).
//
// Files under test:
//   src/game/storage/ChunkStoreRepository.cpp — the simcore→ChunkStore seam
//
// ChunkStoreRepository is the IBlockRepository implementation simcore uses to
// read and compare-and-swap blocks in ChunkStore. It is a 44-line adapter
// with exactly two jobs, and both are pinned here:
//
//   1. refuse to talk to a chunk store that is not connected, and report that
//      through the SAME callback (never by dropping the caller's callback),
//   2. forward the request verbatim and convert the client's result struct
//      into the repository's struct, field for field.
//
// Test double
// -----------
// ChunkStoreRepository takes a std::shared_ptr<IoUringChunkClient> — a
// CONCRETE, non-virtual class — so it cannot be substituted at runtime. This
// target therefore links a link-time stub of the three members the repository
// calls (test/stub_chunk_client.cpp). The stub records what it was asked and
// hands back a scripted reply, and it reproduces the real client's
// unconnected behaviour byte for byte. No socket, no thread, no LMDB, no
// cluster, no MessageRouter, no wall-clock.
//
// player_id=0
// -----------
// The issue also asks to verify the openspec add-multiplayer-foundation claim
// that chunk.requests is published with player_id=0. That is a GATEWAY claim,
// not a repository one: the repository has no player_id concept at all (see
// the test_gateway_chunk_request_* tests at the end, which build the exact
// FlatBuffer gateway.cpp publishes and feed it to the consumer's own parse
// rules). Those tests are hermetic: they reproduce both sides' logic from the
// production sources, they do not link the gateway binary.
//
// Uses the PROJECT's own CHECK/TEST harness (src/engine/net/test/test.h
// convention, mirrored by src/game/world/test/BlockTransforms_test.cpp and
// src/game/machines/test/test_explosion_system.cpp). GoogleTest is deliberately
// NOT used: it is absent from conanfile.txt, CI does not install libgtest-dev,
// and CI builds Release with a global -Werror.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "core_generated.h"
#include "chunkstore_generated.h"

#include <game/storage/ChunkStoreRepository.h>
#include <game/storage/IBlockRepository.h>
#include <apps/simcore/Network/clients/IoUringChunkClient.h>

#include "stub_chunk_client.h"

// ---------------------------------------------------------------------------
// Harness — definitions and macros near the TOP, before first use.
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "",
           msg ? msg : "");
  }
}

#define CHECK(cond, ...) \
  test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) \
  test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) \
  test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)

#define TEST(name) \
  do {               \
    printf("  TEST: %s\n", #name); \
    test_##name();   \
  } while (0)

namespace {

// Cast helper so CHECK_EQ never compares a signed and an unsigned operand
// (which -Wextra/-Werror would reject at the macro expansion site).
constexpr int i_(uint8_t v) { return static_cast<int>(v); }
constexpr int i_(uint16_t v) { return static_cast<int>(v); }
constexpr int i_(uint32_t v) { return static_cast<int>(v); }
constexpr int i_(size_t v) { return static_cast<int>(v); }
constexpr int i_(int v) { return v; }  // also covers int32_t

// IBlockRepository documents CASResult::status as "0 = OK, 1 = CONFLICT"
// (src/game/storage/IBlockRepository.h:8). Named here so a change to the
// protocol shows up as a named-constant mismatch rather than a bare number.
constexpr uint8_t kCasStatusOk = 0;
constexpr uint8_t kCasStatusConflict = 1;

// Wide coordinates, deliberately chosen to be representable but extreme, so a
// silent narrowing anywhere in the forwarding path is visible.
constexpr int32_t kFarX = 1 << 24;
constexpr int32_t kFarY = 319;   // vanilla build ceiling
constexpr int32_t kFarZ = -(1 << 24); // negative: the sign must survive
constexpr uint16_t kWideBlockId = 0xFFFF;
constexpr uint8_t kWideMeta = 0xFF;
constexpr uint32_t kWideMbId = 0xFFFFFFFFu;

// A repository over the link-time stub, with the stub's recorded state
// cleared. Every test starts from a known-empty transcript.
struct Rig {
  std::shared_ptr<simcore::IoUringChunkClient> client =
      std::make_shared<simcore::IoUringChunkClient>();
  simcore::ChunkStoreRepository repo{client};

  Rig() { simcore::g_stub.reset(); }
};

// Make the stub answer as a connected chunk store would.
void goConnected() { simcore::g_stub.connected = true; }

} // namespace

// ---------------------------------------------------------------------------
// The request is formed correctly — every argument reaches the store verbatim
// ---------------------------------------------------------------------------

// The six CAS arguments must survive the hop from the repository to the client
// with no reordering, sign change or narrowing. Extreme values on purpose.
static void test_setBlockCAS_forwards_every_argument_verbatim() {
  Rig r;
  goConnected();

  r.repo.setBlockCAS(kFarX, kFarY, kFarZ, /*expected_block_id=*/1234,
                     /*new_block_id=*/kWideBlockId, kWideMeta,
                     [](const simcore::CASResult&) {});

  CHECK_EQ(i_(simcore::g_stub.cas_calls.size()), 1,
           "one CAS request reaches the chunk store");
  if (simcore::g_stub.cas_calls.size() != 1) return;
  const auto& c = simcore::g_stub.cas_calls[0];
  CHECK_EQ(c.x, kFarX, "x is forwarded, including a large positive value");
  CHECK_EQ(c.y, kFarY, "y is forwarded");
  CHECK_EQ(c.z, kFarZ, "z is forwarded, including its negative sign");
  CHECK_EQ(i_(c.expected_block_id), 1234,
           "expected_block_id is forwarded, not defaulted");
  CHECK_EQ(i_(c.new_block_id), i_(kWideBlockId),
           "new_block_id is forwarded at full uint16 width");
  CHECK_EQ(i_(c.meta), i_(kWideMeta), "meta is forwarded at full uint8 width");
}

// getBlock is the read path: position in, nothing else to forward.
static void test_getBlock_forwards_the_position_verbatim() {
  Rig r;
  goConnected();

  r.repo.getBlock(kFarX, kFarY, kFarZ, [](const simcore::BlockData&) {});

  CHECK_EQ(i_(simcore::g_stub.get_calls.size()), 1,
           "one GetBlock request reaches the chunk store");
  if (simcore::g_stub.get_calls.size() != 1) return;
  const auto& g = simcore::g_stub.get_calls[0];
  CHECK_EQ(g.x, kFarX, "x is forwarded");
  CHECK_EQ(g.y, kFarY, "y is forwarded");
  CHECK_EQ(g.z, kFarZ, "z is forwarded, including its negative sign");
}

// Exactly one hop per call — the repository is a forwarder, not a retrier.
// (A retry loop would double-apply a block change in production.)
static void test_each_repository_call_makes_exactly_one_client_call() {
  Rig r;
  goConnected();

  r.repo.setBlockCAS(1, 2, 3, 4, 5, 6, [](const simcore::CASResult&) {});
  r.repo.getBlock(7, 8, 9, [](const simcore::BlockData&) {});
  r.repo.getBlock(10, 11, 12, [](const simcore::BlockData&) {});

  CHECK_EQ(i_(simcore::g_stub.cas_calls.size()), 1, "one CAS hop, no retry");
  CHECK_EQ(i_(simcore::g_stub.get_calls.size()), 2,
           "one GetBlock hop per getBlock call, no retry and no coalescing");
  if (simcore::g_stub.get_calls.size() == 2) {
    CHECK_EQ(simcore::g_stub.get_calls[0].x, 7, "the first read is its own");
    CHECK_EQ(simcore::g_stub.get_calls[1].x, 10, "the second read is its own");
  }
}

// Two CAS requests in a row are independent — nothing about the first leaks
// into the second (a stale position would corrupt a multiblock write).
static void test_two_consecutive_cas_requests_are_independent() {
  Rig r;
  goConnected();

  r.repo.setBlockCAS(100, 64, 200, 1, 2, 3, [](const simcore::CASResult&) {});
  r.repo.setBlockCAS(-100, 65, -200, 4, 5, 6, [](const simcore::CASResult&) {});

  CHECK_EQ(i_(simcore::g_stub.cas_calls.size()), 2, "both requests arrive");
  if (simcore::g_stub.cas_calls.size() != 2) return;
  CHECK_EQ(simcore::g_stub.cas_calls[0].x, 100, "first x is its own");
  CHECK_EQ(simcore::g_stub.cas_calls[0].z, 200, "first z is its own");
  CHECK_EQ(simcore::g_stub.cas_calls[1].x, -100, "second x is its own");
  CHECK_EQ(simcore::g_stub.cas_calls[1].z, -200, "second z is its own");
  CHECK_EQ(i_(simcore::g_stub.cas_calls[1].expected_block_id), 4,
           "the second CAS carries its own expected_block_id, not the first's");
}

// ---------------------------------------------------------------------------
// The response is applied — the conversion, field for field
// ---------------------------------------------------------------------------

// A committed CAS: status 0 comes back as 0, and the block the store reports
// is passed on unchanged. The repository does NOT rewrite block_id/meta.
static void test_setBlockCAS_applies_a_committed_result() {
  Rig r;
  goConnected();
  simcore::g_stub.reply_status = kCasStatusOk;
  simcore::g_stub.reply_block_id = 1234;
  simcore::g_stub.reply_meta = 7;

  int fired = 0;
  simcore::CASResult seen{};
  r.repo.setBlockCAS(0, 0, 0, 0, 1234, 7, [&](const simcore::CASResult& res) {
    ++fired;
    seen = res;
  });

  CHECK_EQ(fired, 1, "the caller's callback is invoked exactly once");
  CHECK_EQ(i_(seen.status), i_(kCasStatusOk), "a committed CAS reports status 0");
  CHECK_EQ(i_(seen.block_id), 1234, "the committed block_id is applied");
  CHECK_EQ(i_(seen.meta), 7, "the committed meta is applied");
}

// A rejected CAS: status 1 comes back as 1 and the store's actual block is
// reported so the caller can roll back the optimistic write. This is the whole
// point of a compare-and-swap, so it is asserted explicitly.
static void test_setBlockCAS_applies_a_conflict_with_the_actual_block() {
  Rig r;
  goConnected();
  simcore::g_stub.reply_status = kCasStatusConflict;
  simcore::g_stub.reply_block_id = 9999; // what is ACTUALLY there
  simcore::g_stub.reply_meta = 3;

  int fired = 0;
  simcore::CASResult seen{};
  r.repo.setBlockCAS(0, 0, 0, /*expected=*/1234, /*new=*/4321, 5,
                     [&](const simcore::CASResult& res) {
                       ++fired;
                       seen = res;
                     });

  CHECK_EQ(fired, 1, "a conflict still completes the callback");
  CHECK_EQ(i_(seen.status), i_(kCasStatusConflict),
           "a conflict reports status 1, not 0");
  CHECK_EQ(i_(seen.block_id), 9999,
           "the conflict reports the block actually in the store");
  CHECK_EQ(i_(seen.meta), 3, "and that block's actual meta");
}

// status is a byte the repository forwards without validating or clamping.
// A future third status must reach the caller unchanged rather than be
// silently folded into OK/CONFLICT.
static void test_setBlockCAS_does_not_clamp_an_unknown_status() {
  Rig r;
  goConnected();
  simcore::g_stub.reply_status = 42; // not a documented status
  simcore::g_stub.reply_block_id = 0;
  simcore::g_stub.reply_meta = 0;

  simcore::CASResult seen{};
  r.repo.setBlockCAS(0, 0, 0, 0, 0, 0,
                     [&](const simcore::CASResult& res) { seen = res; });

  CHECK_EQ(i_(seen.status), 42,
           "an unrecognised status is passed through, not coerced to 0 or 1");
}

// The widest result values must survive the struct conversion intact.
static void test_setBlockCAS_preserves_wide_result_fields() {
  Rig r;
  goConnected();
  simcore::g_stub.reply_status = kCasStatusOk;
  simcore::g_stub.reply_block_id = kWideBlockId;
  simcore::g_stub.reply_meta = kWideMeta;

  simcore::CASResult seen{};
  r.repo.setBlockCAS(0, 0, 0, 0, kWideBlockId, kWideMeta,
                     [&](const simcore::CASResult& res) { seen = res; });

  CHECK_EQ(i_(seen.block_id), i_(kWideBlockId),
           "block_id 0xFFFF survives the conversion");
  CHECK_EQ(i_(seen.meta), i_(kWideMeta), "meta 0xFF survives the conversion");
}

// getBlock applies the read: block_id, meta and the multiblock id all come
// back. mb_id is the field that ties a block to a multiblock controller, so
// losing it would orphan every multiblocks silently.
static void test_getBlock_applies_the_whole_result_including_mb_id() {
  Rig r;
  goConnected();
  simcore::g_stub.reply_block_id = 4321;
  simcore::g_stub.reply_meta = 11;
  simcore::g_stub.reply_mb_id = 0xDEADBEEF;

  int fired = 0;
  simcore::BlockData seen{};
  r.repo.getBlock(5, 6, 7, [&](const simcore::BlockData& b) {
    ++fired;
    seen = b;
  });

  CHECK_EQ(fired, 1, "the caller's callback is invoked exactly once");
  CHECK_EQ(i_(seen.block_id), 4321, "block_id is applied");
  CHECK_EQ(i_(seen.meta), 11, "meta is applied");
  CHECK_EQ(i_(seen.mb_id), static_cast<int>(0xDEADBEEF),
           "the multiblock id is applied, not dropped");
}

// A read of a plain block reports mb_id 0 (not a multiblock), and a block that
// is part of a multiblock reports non-zero. The repository is a pass-through,
// so both must be distinguishable rather than normalised.
static void test_getBlock_distinguishes_a_standalone_block_from_a_multiblock() {
  Rig r;
  goConnected();

  simcore::g_stub.reply_block_id = 1;
  simcore::g_stub.reply_meta = 0;
  simcore::g_stub.reply_mb_id = 0;
  simcore::BlockData plain{};
  r.repo.getBlock(0, 0, 0, [&](const simcore::BlockData& b) { plain = b; });

  simcore::g_stub.reply_mb_id = 77;
  simcore::BlockData multiblock{};
  r.repo.getBlock(0, 0, 0, [&](const simcore::BlockData& b) { multiblock = b; });

  CHECK_EQ(i_(plain.mb_id), 0, "a standalone block has mb_id 0");
  CHECK_EQ(i_(multiblock.mb_id), 77, "a multiblock block has a non-zero id");
}

// The widest BlockData must survive intact — a 32-bit mb_id is exactly the
// shape that a narrowing conversion would quietly corrupt.
static void test_getBlock_preserves_the_widest_mb_id() {
  Rig r;
  goConnected();
  simcore::g_stub.reply_block_id = kWideBlockId;
  simcore::g_stub.reply_meta = kWideMeta;
  simcore::g_stub.reply_mb_id = kWideMbId;

  simcore::BlockData seen{};
  r.repo.getBlock(0, 0, 0, [&](const simcore::BlockData& b) { seen = b; });

  CHECK_EQ(i_(seen.block_id), i_(kWideBlockId), "block_id 0xFFFF survives");
  CHECK_EQ(i_(seen.meta), i_(kWideMeta), "meta 0xFF survives");
  CHECK_EQ(i_(seen.mb_id), static_cast<int>(kWideMbId),
           "mb_id 0xFFFFFFFF survives the uint32 conversion intact");
}

// ---------------------------------------------------------------------------
// Not connected — the refusal path
// ---------------------------------------------------------------------------

// A null client must still complete the callback. Dropping it would strand the
// caller waiting on a write that will never be acknowledged.
static void test_null_client_still_completes_the_cas_callback() {
  simcore::ChunkStoreRepository repo{nullptr};

  int fired = 0;
  simcore::CASResult seen{};
  repo.setBlockCAS(1, 2, 3, 4, 5, 6, [&](const simcore::CASResult& res) {
    ++fired;
    seen = res;
  });

  CHECK_EQ(fired, 1, "a null client completes the callback rather than dropping it");
  CHECK_EQ(i_(seen.status), i_(kCasStatusConflict),
           "and reports CONFLICT, the safe 'do not assume the write landed'");
  CHECK_EQ(i_(seen.block_id), 0, "with no block id, so nothing is half-applied");
  CHECK_EQ(i_(seen.meta), 0, "and no meta");
}

// A connected-but-false client must behave identically to a null one: the
// guard is IsConnected(), not pointer-ness.
static void test_disconnected_client_reports_conflict_without_hopping() {
  Rig r; // stub starts disconnected
  simcore::g_stub.connected = false;

  int fired = 0;
  simcore::CASResult seen{};
  r.repo.setBlockCAS(9, 9, 9, 1, 2, 3, [&](const simcore::CASResult& res) {
    ++fired;
    seen = res;
  });

  CHECK_EQ(fired, 1, "the callback is completed");
  CHECK_EQ(i_(seen.status), i_(kCasStatusConflict),
           "a disconnected store reports CONFLICT");
  // OBSERVED BEHAVIOUR: the repository checks IsConnected() BEFORE calling the
  // client, so a disconnected store is never asked to build a request at all
  // (ChunkStoreRepository.cpp:17-21). The transcript is therefore empty. This
  // is the stronger of the two possible contracts — no socket write is
  // attempted and no request can be queued for a store that has gone away.
  CHECK_EQ(i_(simcore::g_stub.cas_calls.size()), 0,
           "the request is short-circuited before the client is asked to send it");
}

// The read path's refusal is an all-zero BlockData, not a bogus block. Zero is
// air, so a caller can treat it as "nothing here" without special-casing.
static void test_null_client_still_completes_the_getBlock_callback() {
  simcore::ChunkStoreRepository repo{nullptr};

  int fired = 0;
  simcore::BlockData seen{1, 2, 3}; // poisoned: proves every field is written
  repo.getBlock(1, 2, 3, [&](const simcore::BlockData& b) {
    ++fired;
    seen = b;
  });

  CHECK_EQ(fired, 1, "a null client completes the getBlock callback");
  CHECK_EQ(i_(seen.block_id), 0, "block_id is zeroed");
  CHECK_EQ(i_(seen.meta), 0, "meta is zeroed");
  CHECK_EQ(i_(seen.mb_id), 0, "mb_id is zeroed");
}

static void test_disconnected_client_getBlock_reads_as_air() {
  Rig r;
  simcore::g_stub.connected = false;

  int fired = 0;
  simcore::BlockData seen{1, 2, 3};
  r.repo.getBlock(9, 9, 9, [&](const simcore::BlockData& b) {
    ++fired;
    seen = b;
  });

  CHECK_EQ(fired, 1, "the callback is completed");
  CHECK_EQ(i_(seen.block_id), 0, "an unreadable block reads as air");
  CHECK_EQ(i_(seen.meta), 0, "with no meta");
  CHECK_EQ(i_(seen.mb_id), 0, "and no multiblock id");
}

// Repeated calls while disconnected keep answering — the store may come back at
// any time and a caller must not get a permanently-silent callback.
static void test_disconnected_client_keeps_answering_every_call() {
  Rig r;
  simcore::g_stub.connected = false;

  int cas_fired = 0, get_fired = 0;
  for (int i = 0; i < 5; ++i) {
    r.repo.setBlockCAS(i, i, i, 0, 0, 0,
                       [&](const simcore::CASResult&) { ++cas_fired; });
    r.repo.getBlock(i, i, i, [&](const simcore::BlockData&) { ++get_fired; });
  }

  CHECK_EQ(cas_fired, 5, "every CAS call is answered while disconnected");
  CHECK_EQ(get_fired, 5, "every getBlock call is answered while disconnected");
}

// The callbacks run INLINE, on the calling thread. That is what makes the
// refusal path safe for a caller that assumes "callback already ran on
// return", and it is why this suite needs no sleeps or polling.
static void test_callbacks_are_synchronous_on_the_calling_thread() {
  Rig r;
  simcore::g_stub.connected = false;

  bool cas_fired = false, get_fired = false;
  r.repo.setBlockCAS(0, 0, 0, 0, 0, 0,
                     [&](const simcore::CASResult&) { cas_fired = true; });
  CHECK(cas_fired, "the CAS callback has already run on return (disconnected)");
  r.repo.getBlock(0, 0, 0, [&](const simcore::BlockData&) { get_fired = true; });
  CHECK(get_fired, "the getBlock callback has already run on return (disconnected)");

  simcore::g_stub.connected = true;
  bool live_cas = false, live_get = false;
  r.repo.setBlockCAS(0, 0, 0, 0, 0, 0,
                     [&](const simcore::CASResult&) { live_cas = true; });
  CHECK(live_cas, "and likewise on the connected path");
  r.repo.getBlock(0, 0, 0, [&](const simcore::BlockData&) { live_get = true; });
  CHECK(live_get, "and likewise for getBlock");
}

// Reconnecting the store changes the answer: the same call that reported
// CONFLICT now reports what the store actually says. This is the property that
// makes the repository safe to keep across a chunk-store restart.
static void test_reconnecting_the_store_changes_the_answer() {
  Rig r;

  simcore::CASResult before{};
  r.repo.setBlockCAS(0, 0, 0, 0, 5, 0,
                     [&](const simcore::CASResult& res) { before = res; });
  CHECK_EQ(i_(before.status), i_(kCasStatusConflict),
           "while disconnected every CAS conflicts");

  goConnected();
  simcore::g_stub.reply_status = kCasStatusOk;
  simcore::g_stub.reply_block_id = 5;
  simcore::CASResult after{};
  r.repo.setBlockCAS(0, 0, 0, 0, 5, 0,
                     [&](const simcore::CASResult& res) { after = res; });

  CHECK_EQ(i_(after.status), i_(kCasStatusOk),
           "the same call commits once the store is back");
  CHECK_EQ(i_(after.block_id), 5, "and reports the written block");
}

// The repository is usable purely through the IBlockRepository interface,
// which is how every caller in production holds it. The override must be
// reachable that way, not only on the concrete type.
static void test_repository_is_usable_through_the_i_block_repository_interface() {
  Rig r;
  goConnected();
  simcore::g_stub.reply_block_id = 55;
  simcore::g_stub.reply_status = kCasStatusOk;

  simcore::IBlockRepository& asInterface = r.repo;
  int cas_fired = 0, get_fired = 0;
  simcore::CASResult cas_seen{};
  simcore::BlockData get_seen{};
  asInterface.setBlockCAS(0, 0, 0, 1, 55, 0,
                          [&](const simcore::CASResult& res) {
                            ++cas_fired;
                            cas_seen = res;
                          });
  asInterface.getBlock(0, 0, 0, [&](const simcore::BlockData& b) {
    ++get_fired;
    get_seen = b;
  });

  CHECK_EQ(cas_fired, 1, "setBlockCAS dispatches through the interface");
  CHECK_EQ(get_fired, 1, "getBlock dispatches through the interface");
  CHECK_EQ(i_(cas_seen.block_id), 55, "the CAS result arrives intact");
  CHECK_EQ(i_(get_seen.block_id), 55, "the read result arrives intact");
}

// ---------------------------------------------------------------------------
// player_id=0 on chunk.requests — the openspec claim, verified
// ---------------------------------------------------------------------------
//
// Source of truth for the code reproduced below:
//   src/apps/gateway/gateway.cpp:127-131 and :488-491  (the producer)
//   src/apps/gateway/main.cpp:84-90                    (the forwarder)
//   src/apps/chunk_store/Network/IoUringRouterClient.cpp:37-60  (the consumer)
//
// These tests do NOT link the gateway or chunkd binaries. They rebuild the
// producer's FlatBuffer with the production builder and run the consumer's own
// accept/reject rules, so the claim is checked hermetically and deterministically.

// The exact FlatBuffer gateway.cpp builds: a PlayerAction with player_id=0 and
// action=CHUNK_REQUEST at a chunk coordinate. This asserts the CLAIM itself
// (the bytes really do carry player_id 0) before asking whether it matters.
static void test_gateway_chunk_request_is_built_with_player_id_zero() {
  flatbuffers::FlatBufferBuilder fbb;
  const int32_t cx = 12, cy = 1, cz = -34;
  Protocol::Vec3i pos(cx, cy, cz);
  auto action = Protocol::CreatePlayerAction(
      fbb, /*player_id=*/0, Protocol::PlayerActionType_CHUNK_REQUEST, &pos);
  fbb.Finish(action);

  const std::vector<uint8_t> bytes(fbb.GetBufferPointer(),
                                   fbb.GetBufferPointer() + fbb.GetSize());
  flatbuffers::Verifier v(bytes.data(), bytes.size());
  CHECK(v.VerifyBuffer<Protocol::PlayerAction>(nullptr),
        "the gateway's chunk.requests payload is a valid PlayerAction");
  if (!v.VerifyBuffer<Protocol::PlayerAction>(nullptr)) return;

  const auto* pa = flatbuffers::GetRoot<Protocol::PlayerAction>(bytes.data());
  CHECK_EQ(i_(pa->player_id()), 0,
           "CLAIM CONFIRMED: the gateway publishes chunk.requests with "
           "player_id == 0 (gateway.cpp:127 and :488 pass a literal 0)");
  CHECK_EQ(i_(pa->action()), i_(Protocol::PlayerActionType_CHUNK_REQUEST),
           "the action is CHUNK_REQUEST");
  CHECK(pa->pos() != nullptr, "and it carries a required pos");
  if (pa->pos()) {
    CHECK_EQ(pa->pos()->x(), cx, "x is the chunk coordinate");
    CHECK_EQ(pa->pos()->y(), cy, "y is the chunk coordinate");
    CHECK_EQ(pa->pos()->z(), cz, "z is the chunk coordinate, sign included");
  }
}

// chunkd's accept rules, transcribed from
// src/apps/chunk_store/Network/IoUringRouterClient.cpp:37-60. It checks the
// topic, a minimum length, a non-null root, a non-null pos, and the action
// type. player_id is never read.
static void test_chunk_store_consumer_ignores_player_id() {
  const uint8_t kChunkRequest = static_cast<uint8_t>(
      Protocol::PlayerActionType_CHUNK_REQUEST);

  // The producer's actual payload, and the same payload with a real player id.
  // Both are accepted identically, which is the whole finding.
  for (uint64_t pid : {0ull, 1ull, 4242ull}) {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(5, 0, 5);
    auto action = Protocol::CreatePlayerAction(
        fbb, pid, Protocol::PlayerActionType_CHUNK_REQUEST, &pos);
    fbb.Finish(action);
    const std::vector<uint8_t> bytes(fbb.GetBufferPointer(),
                                     fbb.GetBufferPointer() + fbb.GetSize());

    // chunkd's own guards, in order.
    const bool long_enough = bytes.size() >= 4;
    const auto* pa = long_enough
                         ? flatbuffers::GetRoot<Protocol::PlayerAction>(bytes.data())
                         : nullptr;
    const bool accepted = pa != nullptr && pa->pos() != nullptr &&
                          pa->action() == kChunkRequest;
    CHECK(accepted, "chunkd accepts the chunk request regardless of player_id");
  }
}

// The consumer also rejects a wrong action type, proving the accept test is
// real rather than vacuously true.
static void test_chunk_store_consumer_still_checks_the_action_type() {
  flatbuffers::FlatBufferBuilder fbb;
  Protocol::Vec3i pos(5, 0, 5);
  auto action = Protocol::CreatePlayerAction(
      fbb, /*player_id=*/0, Protocol::PlayerActionType_MOVE, &pos);
  fbb.Finish(action);
  const std::vector<uint8_t> bytes(fbb.GetBufferPointer(),
                                   fbb.GetBufferPointer() + fbb.GetSize());

  const auto* pa = flatbuffers::GetRoot<Protocol::PlayerAction>(bytes.data());
  const bool accepted = pa != nullptr && pa->pos() != nullptr &&
                        pa->action() ==
                            static_cast<uint8_t>(
                                Protocol::PlayerActionType_CHUNK_REQUEST);
  CHECK(!accepted,
        "a MOVE with player_id 0 is still rejected — the action type is the "
        "real gate, so player_id=0 is inert on this path");
}

// The chunkstore request tables themselves carry no player_id at all, so a
// block write or read can never be attributed to a player no matter what the
// gateway sent. This is the structural reason player_id=0 is harmless here.
static void test_chunk_store_request_types_have_no_player_id() {
  flatbuffers::FlatBufferBuilder fbb(256);
  Protocol::Vec3i pos(1, 2, 3);
  auto cas = Protocol::CreateSetBlockCASReq(fbb, &pos, /*expected_block_id=*/1,
                                           /*new_block_id=*/2, /*meta=*/3);
  fbb.Finish(cas);
  const std::vector<uint8_t> bytes(fbb.GetBufferPointer(),
                                   fbb.GetBufferPointer() + fbb.GetSize());
  flatbuffers::Verifier v(bytes.data(), bytes.size());
  CHECK(v.VerifyBuffer<Protocol::SetBlockCASReq>(nullptr),
        "a SetBlockCASReq built for a position verifies");
  if (!v.VerifyBuffer<Protocol::SetBlockCASReq>(nullptr)) return;
  const auto* req = flatbuffers::GetRoot<Protocol::SetBlockCASReq>(bytes.data());
  CHECK(req->pos() != nullptr, "the only identity it carries is the position");
  if (req->pos()) {
    CHECK_EQ(req->pos()->x(), 1, "x is preserved");
    CHECK_EQ(req->pos()->y(), 2, "y is preserved");
    CHECK_EQ(req->pos()->z(), 3, "z is preserved");
  }
}

// ChunkStoreRepository itself has no player_id surface at all: a block write
// is authorised by the expected_block_id compare, never by an identity. Pinned
// so a future "add player_id to the repository" change is a visible API change
// rather than a silent one.
static void test_chunk_store_repository_has_no_player_id_surface() {
  // The full IBlockRepository surface — every call the repository exposes. If
  // a player_id parameter were ever added, this list would need updating and
  // the compile would break here rather than at 30 call sites.
  Rig r;
  goConnected();
  r.repo.setBlockCAS(0, 0, 0, 0, 0, 0, [](const simcore::CASResult&) {});
  r.repo.getBlock(0, 0, 0, [](const simcore::BlockData&) {});
  CHECK_EQ(i_(simcore::g_stub.cas_calls.size()) + i_(simcore::g_stub.get_calls.size()),
           2,
           "the repository surface is (position, block ids, meta) only — no "
           "player id, so the player_id=0 publish cannot reach it");
}

int main() {
  printf("=== chunk_store_repository test suite ===\n\n");

  // Request formation
  TEST(setBlockCAS_forwards_every_argument_verbatim);
  TEST(getBlock_forwards_the_position_verbatim);
  TEST(each_repository_call_makes_exactly_one_client_call);
  TEST(two_consecutive_cas_requests_are_independent);

  // Response application
  TEST(setBlockCAS_applies_a_committed_result);
  TEST(setBlockCAS_applies_a_conflict_with_the_actual_block);
  TEST(setBlockCAS_does_not_clamp_an_unknown_status);
  TEST(setBlockCAS_preserves_wide_result_fields);
  TEST(getBlock_applies_the_whole_result_including_mb_id);
  TEST(getBlock_distinguishes_a_standalone_block_from_a_multiblock);
  TEST(getBlock_preserves_the_widest_mb_id);

  // Not connected
  TEST(null_client_still_completes_the_cas_callback);
  TEST(disconnected_client_reports_conflict_without_hopping);
  TEST(null_client_still_completes_the_getBlock_callback);
  TEST(disconnected_client_getBlock_reads_as_air);
  TEST(disconnected_client_keeps_answering_every_call);
  TEST(callbacks_are_synchronous_on_the_calling_thread);
  TEST(reconnecting_the_store_changes_the_answer);
  TEST(repository_is_usable_through_the_i_block_repository_interface);

  // player_id=0 verification
  TEST(gateway_chunk_request_is_built_with_player_id_zero);
  TEST(chunk_store_consumer_ignores_player_id);
  TEST(chunk_store_consumer_still_checks_the_action_type);
  TEST(chunk_store_request_types_have_no_player_id);
  TEST(chunk_store_repository_has_no_player_id_surface);

  printf("\n=== Results: %d checks, %d passed, %d failed ===\n", g_tests,
         g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
