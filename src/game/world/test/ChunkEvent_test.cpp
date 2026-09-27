// Unit tests for the chunk-event ingestion path (issue gp-3mq4):
//
//   src/game/world/ChunkSnapshot.cpp    — snapshot construction
//   src/game/world/ChunkEventHandler.cpp — event validation and dispatch order
//
// These tests assert the OBSERVED behaviour of the two units as they exist
// today. Two of them pin quirks that look like defects but are not being
// "fixed" here (see the OBSERVED QUIRK / DEFECT notes in place); a defect
// report is filed separately rather than papered over in the test.
//
// Harness: the project's own CHECK/TEST macros (src/engine/net/test/test.h,
// mirrored by the sibling src/game/world/test/BlockTransforms_test.cpp). The
// repo has zero GoogleTest usage, gtest is absent from conanfile.txt and CI
// does not install it, so a find_package(GTest) guard would silently drop the
// test on CI instead of failing it.

#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>

#include <engine/sim/SimulationEngine.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/Position.h>

#include <game/world/ChunkEventHandler.h>
#include <game/world/ChunkSnapshot.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

// ---- project test harness (mirrors src/game/world/test/BlockTransforms_test.cpp) ----
int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg = nullptr) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    printf("  FAIL %s:%d: %s%s%s\n", file, line, expr, msg ? " -- " : "", msg ? msg : "");
  }
}

#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)
#define CHECK_GE(a, b, ...) test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, ##__VA_ARGS__)

#define TEST(name) do { printf("  TEST: %s\n", #name); test_##name(); } while (0)

namespace {

using simcore::Block;
using simcore::ChunkEventHandler;
using simcore::ChunkSnapshot;
using simcore::Position;
using simcore::SimulationEngine;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

// Serializes exactly what ChunkStore publishes on "world.blocks.changed": a
// Protocol::BlockChangedEvent rooted flatbuffer.
std::vector<uint8_t> makeEvent(int32_t x, int32_t y, int32_t z, uint16_t block_id,
                               uint8_t meta, uint32_t mb_id, uint32_t request_id = 0,
                               uint64_t source_player_id = 0) {
  flatbuffers::FlatBufferBuilder fbb(64);
  Protocol::Vec3i pos(x, y, z);
  fbb.Finish(Protocol::CreateBlockChangedEvent(fbb, &pos, block_id, meta, mb_id,
                                               request_id, source_player_id));
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

// Number of ECS entities carrying a Position at (x,y,z).
size_t countAt(SimulationEngine& engine, uint32_t x, uint32_t y, uint32_t z) {
  size_t n = 0;
  auto view = engine.reg().view<const Position>();
  for (auto entity : view) {
    const auto& pos = view.get<const Position>(entity);
    if (pos.x == x && pos.y == y && pos.z == z) ++n;
  }
  return n;
}

// Number of entities with a Position at all.
size_t positionCount(SimulationEngine& engine) {
  return engine.reg().view<const Position>().size();
}

// The Block at (x,y,z), or nullptr when the position holds no block.
const Block* blockAt(SimulationEngine& engine, uint32_t x, uint32_t y, uint32_t z) {
  auto view = engine.reg().view<const Position, const Block>();
  for (auto entity : view) {
    auto [pos, block] = view.get(entity);
    if (pos.x == x && pos.y == y && pos.z == z) return &block;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// ChunkSnapshot: construction
// ---------------------------------------------------------------------------

// A well-formed BlockChangedEvent yields the position of the event and the
// raw payload is kept byte-for-byte.
static void test_SnapshotReadsCoordFromEventPos() {
  const auto buf = makeEvent(-5, 7, -9, 42, 3, 11);
  const ChunkSnapshot snapshot(buf.data(), buf.size());

  const auto coord = snapshot.coord();
  CHECK_EQ(coord.x, -5);
  CHECK_EQ(coord.y, 7);
  CHECK_EQ(coord.z, -9);
  CHECK_EQ(snapshot.size(), buf.size());
  CHECK_NE(snapshot.data(), nullptr);
  CHECK_EQ(snapshot.data()[0], buf[0]);
}

// coord() keeps the SIGNED value the wire format carries.
static void test_SnapshotCoordIsSigned() {
  const auto buf = makeEvent(-1, -2, -3, 0, 0, 0);
  const ChunkSnapshot snapshot(buf.data(), buf.size());
  const auto coord = snapshot.coord();
  CHECK(coord.x < 0);
  CHECK_EQ(coord.x, -1);
  CHECK_EQ(coord.y, -2);
  CHECK_EQ(coord.z, -3);
}

// The payload is deep-copied: scribbling over the caller's buffer afterwards
// does not change the snapshot's own bytes.
static void test_SnapshotOwnsACopyOfThePayload() {
  auto buf = makeEvent(1, 2, 3, 4, 5, 6);
  const ChunkSnapshot snapshot(buf.data(), buf.size());

  for (auto& byte : buf) byte = 0xEE;
  CHECK_EQ(snapshot.size(), buf.size());
  CHECK_NE(snapshot.data(), buf.data(), "snapshot must not alias the caller's buffer");
  CHECK_NE(snapshot.data()[0], 0xEE);
}

// A zero-length snapshot is the documented "empty snapshot" case: zeroed
// coordinates, no payload. (This is the only path the `if (!snapshot)` guard
// in ChunkSnapshot.cpp can actually reach, because GetRoot of a 0-byte buffer
// still returns a non-null pointer.)
static void test_EmptyPayloadYieldsZeroedSnapshot() {
  const uint8_t* null_data = nullptr;
  const ChunkSnapshot from_null(null_data, 0);
  CHECK_EQ(from_null.size(), 0u);
  CHECK(from_null.data() == nullptr);
  CHECK_EQ(from_null.coord().x, 0);
  CHECK_EQ(from_null.coord().y, 0);
  CHECK_EQ(from_null.coord().z, 0);

  const std::vector<uint8_t> empty;
  const ChunkSnapshot from_empty(empty.data(), empty.size());
  CHECK_EQ(from_empty.size(), 0u);
  CHECK_EQ(from_empty.coord().x, 0);
  CHECK_EQ(from_empty.coord().z, 0);
}

// OBSERVED QUIRK: blocks/meta/multiblock are never populated — the parsing of
// those three vectors is commented out in ChunkSnapshot.cpp. Every getter
// therefore returns 0 for EVERY coordinate, whatever the event carried, and
// the out-of-range fallback is unreachable. Pinned so re-enabling the parsing
// has to update this test on purpose.
static void test_GettersAlwaysReturnZero() {
  const auto buf = makeEvent(1, 2, 3, 4242, 9, 777);
  const ChunkSnapshot snapshot(buf.data(), buf.size());

  // The event really does carry nonzero block/meta/mb_id...
  const auto* event = flatbuffers::GetRoot<Protocol::BlockChangedEvent>(snapshot.data());
  CHECK_EQ(event->block_id(), 4242u);
  CHECK_EQ(event->meta(), 9u);
  CHECK_EQ(event->mb_id(), 777u);
  // ...but the snapshot exposes none of it.
  CHECK_EQ(snapshot.getBlock(0, 0, 0), 0u);
  CHECK_EQ(snapshot.getMeta(0, 0, 0), 0u);
  CHECK_EQ(snapshot.getMultiblock(0, 0, 0), 0u);
  CHECK_EQ(snapshot.getBlock(1, 2, 3), 0u);
  CHECK_EQ(snapshot.getMeta(1, 2, 3), 0u);
  CHECK_EQ(snapshot.getMultiblock(1, 2, 3), 0u);
}

// The same holds at coordinate extremes: xyz() packs 10 bits per axis, so
// larger-than-chunk values just miss the (always empty) backing vector.
static void test_GettersReturnZeroAtCoordinateExtremes() {
  const auto buf = makeEvent(0, 0, 0, 1, 1, 1);
  const ChunkSnapshot snapshot(buf.data(), buf.size());
  CHECK_EQ(snapshot.getBlock(1023, 1023, 1023), 0u);
  CHECK_EQ(snapshot.getMeta(1023, 1023, 1023), 0u);
  CHECK_EQ(snapshot.getMultiblock(0xFFFFu, 0u, 0u), 0u);
}

// Construction is a pure function of the payload: same bytes in, same state.
static void test_ConstructionIsDeterministic() {
  const auto buf = makeEvent(9, -9, 4, 12, 3, 34);
  const ChunkSnapshot first(buf.data(), buf.size());
  const ChunkSnapshot second(buf.data(), buf.size());

  CHECK_EQ(first.coord().x, second.coord().x);
  CHECK_EQ(first.coord().y, second.coord().y);
  CHECK_EQ(first.coord().z, second.coord().z);
  CHECK_EQ(first.size(), second.size());
  CHECK_EQ(first.getBlock(2, 2, 2), second.getBlock(2, 2, 2));
}

// OBSERVED QUIRK: nothing checks the root type, so any flatbuffer whose
// vtable slot 4 is a 12-byte struct is read as a BlockChangedEvent — a
// ChunkData buffer (coord + blocks + meta + multiblock) decodes as
// coord=(7,8,9) instead of being rejected. Unlike ChunkEventHandler, the
// snapshot performs NO verification, so a corrupt buffer is undefined
// behaviour here (see the defect report; not exercised in this test because a
// segfault cannot be asserted on).
static void test_AnyFlatbufferWithAStructAtSlotFourIsAccepted() {
  flatbuffers::FlatBufferBuilder fbb(64);
  Protocol::Vec3i coord(7, 8, 9);
  const std::vector<uint16_t> blocks{11, 22, 33};
  const auto block_vec = fbb.CreateVector(blocks);
  fbb.Finish(Protocol::CreateChunkData(fbb, &coord, block_vec, 0, 0));
  const std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                                 fbb.GetBufferPointer() + fbb.GetSize());

  const ChunkSnapshot snapshot(buf.data(), buf.size());
  CHECK_EQ(snapshot.coord().x, 7);
  CHECK_EQ(snapshot.coord().y, 8);
  CHECK_EQ(snapshot.coord().z, 9);
  // The blocks it was built with are still invisible.
  CHECK_EQ(snapshot.getBlock(0, 0, 0), 0u);
}

// gp-mlcj: garbage off the wire must not become a coordinate.
//
// The test above, AnyFlatbufferWithAStructAtSlotFourIsAccepted, feeds a
// well-formed ChunkData to a constructor that reads a BlockChangedEvent. That
// still passes with a Verifier in place, and it is the honest limit of what
// verification buys: flatbuffers verification is STRUCTURAL, not type-tagged,
// so the wrong message type still verifies. Pinning that here means the gap is
// documented rather than discovered later.
//
// What the Verifier does buy is refusal of bytes that are not a flatbuffer at
// all. GetRoot never returns null - it manufactures a Table* from whatever it
// is handed - so before gp-mlcj the first read of an attacker-controlled offset
// was snapshot->pos() on the next line. These three cases are that, tested.
static void test_GarbageBytesDoNotBecomeACoordinate() {
  // 64 bytes of noise: enough to have a plausible-looking root offset.
  const std::vector<uint8_t> noise(64, 0xAB);
  const ChunkSnapshot a(noise.data(), noise.size());
  CHECK_EQ(a.coord().x, 0);
  CHECK_EQ(a.coord().y, 0);
  CHECK_EQ(a.coord().z, 0);

  // A truncated real buffer: the root offset is past the end.
  flatbuffers::FlatBufferBuilder fbb(64);
  Protocol::Vec3i coord(7, 8, 9);
  const auto block_vec = fbb.CreateVector(std::vector<uint16_t>{11, 22, 33});
  fbb.Finish(Protocol::CreateChunkData(fbb, &coord, block_vec, 0, 0));
  const std::vector<uint8_t> full(fbb.GetBufferPointer(),
                                   fbb.GetBufferPointer() + fbb.GetSize());
  for (size_t cut : {size_t(1), size_t(4), full.size() / 2}) {
    const ChunkSnapshot t(full.data(), cut);
    CHECK_EQ(t.coord().x, 0);
    CHECK_EQ(t.coord().y, 0);
    CHECK_EQ(t.coord().z, 0);
  }
}

// ---------------------------------------------------------------------------
// ChunkEventHandler: validation gate
// ---------------------------------------------------------------------------

// An empty payload is logged and dropped; the engine is untouched.
static void test_EmptyPayloadIsIgnored() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle({});

  CHECK_EQ(positionCount(*engine), 0u);
}

// A payload that fails flatbuffers verification is logged and dropped. Three
// shapes are covered: the ~28-byte wire size with garbage bytes, a valid
// buffer chopped by one byte, and an all-zero buffer.
static void test_InvalidBuffersAreIgnored() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(std::vector<uint8_t>(28, 0xAB));

  const auto full = makeEvent(7, 8, 9, 12, 1, 0);
  std::vector<uint8_t> truncated(full.begin(), full.end() - 1);
  handler.handle(truncated);

  handler.handle(std::vector<uint8_t>(64, 0));

  CHECK_EQ(positionCount(*engine), 0u);
}

// A rejected payload leaves no entity behind and does not prevent the next
// valid event from being applied.
static void test_RejectionDoesNotPoisonLaterEvents() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(std::vector<uint8_t>(28, 0xAB));
  CHECK_EQ(positionCount(*engine), 0u);

  handler.handle(makeEvent(3, 4, 5, 6, 7, 8));

  CHECK_EQ(positionCount(*engine), 1u);
  CHECK_EQ(countAt(*engine, 3, 4, 5), 1u);
  const auto* block = blockAt(*engine, 3, 4, 5);
  CHECK_NE(block, nullptr);
  if (block) {
    CHECK_EQ(block->id, 6u);
    CHECK_EQ(block->meta, 7u);
    CHECK_EQ(block->mb_id, 8u);
  }
}

// ---------------------------------------------------------------------------
// ChunkEventHandler: dispatch into the engine
// ---------------------------------------------------------------------------

// A valid event becomes a block entity carrying the wire values verbatim.
static void test_ValidEventCreatesBlockEntity() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(makeEvent(10, 20, 30, 77, 5, 42));

  CHECK_EQ(positionCount(*engine), 1u);
  const auto* block = blockAt(*engine, 10, 20, 30);
  CHECK_NE(block, nullptr);
  if (block) {
    CHECK_EQ(block->id, 77u);
    CHECK_EQ(block->meta, 5u);
    CHECK_EQ(block->mb_id, 42u);
  }
}

// The handler is stateless with respect to the payload: the caller's vector is
// taken by const reference and is not consumed or rewritten.
static void test_HandlerDoesNotMutateTheInput() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  const auto buf = makeEvent(1, 1, 1, 2, 3, 4);
  const auto before = buf;
  handler.handle(buf);

  CHECK_EQ(buf.size(), before.size());
  CHECK(buf == before);
}

// Distinct positions create distinct entities; nothing is shared or merged.
static void test_DistinctPositionsCreateDistinctEntities() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(makeEvent(1, 1, 1, 100, 1, 0));
  handler.handle(makeEvent(2, 2, 2, 200, 2, 0));
  handler.handle(makeEvent(3, 3, 3, 300, 3, 0));

  CHECK_EQ(positionCount(*engine), 3u);
  const auto* a = blockAt(*engine, 1, 1, 1);
  const auto* b = blockAt(*engine, 2, 2, 2);
  const auto* c = blockAt(*engine, 3, 3, 3);
  CHECK(a && b && c);
  if (a) CHECK_EQ(a->id, 100u);
  if (b) CHECK_EQ(b->id, 200u);
  if (c) CHECK_EQ(c->id, 300u);
}

// A repeat event for the same position updates the EXISTING entity rather than
// creating a second one.
static void test_SamePositionIsUpdatedInPlace() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(makeEvent(10, 20, 30, 77, 5, 0));
  handler.handle(makeEvent(10, 20, 30, 77, 6, 0));
  handler.handle(makeEvent(10, 20, 30, 77, 7, 0));

  CHECK_EQ(positionCount(*engine), 1u);
  CHECK_EQ(countAt(*engine, 10, 20, 30), 1u);
  const auto* block = blockAt(*engine, 10, 20, 30);
  CHECK_NE(block, nullptr);
  if (block) {
    CHECK_EQ(block->id, 77u);
    CHECK_EQ(block->meta, 7u, "last write wins for meta");
  }
}

// OBSERVED QUIRK (documented in SimulationEngine::onBlockChanged): an action
// echo carries mb_id=0, and the engine PRESERVES the multiblock membership
// when the echoed block id is unchanged. Without this a later anchor break
// could not find the owning controller.
static void test_EchoWithZeroMbIdPreservesExistingMbId() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(makeEvent(10, 20, 30, 77, 5, 42));
  const auto* before = blockAt(*engine, 10, 20, 30);
  CHECK_NE(before, nullptr);
  if (before) CHECK_EQ(before->mb_id, 42u);

  // Same block id, mb_id=0, new meta -> membership survives the echo.
  handler.handle(makeEvent(10, 20, 30, 77, 9, 0));

  CHECK_EQ(positionCount(*engine), 1u);
  const auto* after = blockAt(*engine, 10, 20, 30);
  CHECK_NE(after, nullptr);
  if (after) {
    CHECK_EQ(after->id, 77u);
    CHECK_EQ(after->meta, 9u, "meta is applied from the echo");
    CHECK_EQ(after->mb_id, 42u, "mb_id is preserved across an unchanged-block echo");
  }
}

// Changing the block id at a position that belonged to a multiblock drops the
// membership (old_block->mb_id != mb_id), and the entity is still reused.
static void test_ChangingBlockIdResetsMbId() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(makeEvent(10, 20, 30, 77, 5, 42));
  handler.handle(makeEvent(10, 20, 30, 88, 1, 0));

  CHECK_EQ(positionCount(*engine), 1u);
  const auto* block = blockAt(*engine, 10, 20, 30);
  CHECK_NE(block, nullptr);
  if (block) {
    CHECK_EQ(block->id, 88u);
    CHECK_EQ(block->mb_id, 0u, "controller membership is dropped on id change");
  }
}

// block_id == 0 is the "remove to air" event: the Block is dropped but the
// entity and its Position survive, so the position is not re-created later.
static void test_AirEventClearsBlockButKeepsEntity() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(makeEvent(10, 20, 30, 77, 5, 0));
  handler.handle(makeEvent(10, 20, 30, 0, 0, 0));

  CHECK_EQ(blockAt(*engine, 10, 20, 30), nullptr);
  CHECK_EQ(countAt(*engine, 10, 20, 30), 1u);
  CHECK_EQ(positionCount(*engine), 1u);

  // Re-placing the block reuses that same entity.
  handler.handle(makeEvent(10, 20, 30, 77, 6, 0));
  CHECK_EQ(positionCount(*engine), 1u);
  const auto* block = blockAt(*engine, 10, 20, 30);
  if (block) CHECK_EQ(block->meta, 6u);
}

// An air event for a position the engine never saw is a no-op: no entity is
// created for a removal.
static void test_AirEventOnUnknownPositionIsNoOp() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  handler.handle(makeEvent(50, 60, 70, 0, 0, 0));

  CHECK_EQ(positionCount(*engine), 0u);
  CHECK_EQ(countAt(*engine, 50, 60, 70), 0u);
}

// OBSERVED QUIRK: the wire position is int32 but the engine stores uint32, so
// a negative coordinate silently wraps to a huge positive one instead of being
// rejected. Contrast with ChunkSnapshot::coord(), which keeps it signed — the
// same event yields two different coordinates depending on which unit reads it.
static void test_NegativeCoordinatesWrapToUint32() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  const auto buf = makeEvent(-1, -2, -3, 5, 0, 0);

  const ChunkSnapshot snapshot(buf.data(), buf.size());
  CHECK_EQ(snapshot.coord().x, -1);

  handler.handle(buf);

  CHECK_EQ(countAt(*engine, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFDu), 1u);
  CHECK_EQ(countAt(*engine, 0xFFFFFFFFu, 0, 0), 0u);
}

// request_id / source_player_id exist on the wire but are not forwarded to the
// engine, so two otherwise identical events produce identical world state.
static void test_CorrelationFieldsDoNotAffectWorldState() {
  auto plain = std::make_shared<SimulationEngine>();
  auto annotated = std::make_shared<SimulationEngine>();
  ChunkEventHandler plain_handler(plain);
  ChunkEventHandler annotated_handler(annotated);

  plain_handler.handle(makeEvent(5, 6, 7, 8, 9, 10));
  annotated_handler.handle(makeEvent(5, 6, 7, 8, 9, 10, /*request_id=*/4242,
                                     /*source_player_id=*/99));

  const auto* a = blockAt(*plain, 5, 6, 7);
  const auto* b = blockAt(*annotated, 5, 6, 7);
  CHECK_NE(a, nullptr);
  CHECK_NE(b, nullptr);
  if (a && b) {
    CHECK_EQ(a->id, b->id);
    CHECK_EQ(a->meta, b->meta);
    CHECK_EQ(a->mb_id, b->mb_id);
  }
}

// Ordering: a mixed batch of placements, echoes, id changes and removals
// applied in sequence leaves the world in the state implied by the LAST event
// at each position, and no other position is affected.
static void test_MixedBatchIsAppliedInOrder() {
  auto engine = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler(engine);

  // (1) two fresh placements
  handler.handle(makeEvent(1, 1, 1, 100, 0, 0));
  handler.handle(makeEvent(2, 2, 2, 200, 0, 0));
  // (2) a multiblock anchor, then an echo of it
  handler.handle(makeEvent(3, 3, 3, 300, 0, 55));
  handler.handle(makeEvent(3, 3, 3, 300, 7, 0));
  // (3) a rejected payload in the middle of the batch changes nothing
  handler.handle(std::vector<uint8_t>(28, 0xAB));
  // (4) replace the block at (1,1,1) with a different id
  handler.handle(makeEvent(1, 1, 1, 111, 1, 0));
  // (5) remove (2,2,2) to air
  handler.handle(makeEvent(2, 2, 2, 0, 0, 0));
  // (6) a new position after the removal
  handler.handle(makeEvent(4, 4, 4, 400, 4, 0));

  CHECK_EQ(positionCount(*engine), 4u);

  // (1,1,1): last write wins, entity reused.
  const auto* first = blockAt(*engine, 1, 1, 1);
  CHECK_NE(first, nullptr);
  if (first) {
    CHECK_EQ(first->id, 111u);
    CHECK_EQ(first->meta, 1u);
  }
  // (2,2,2): removed, but the entity is still there.
  CHECK_EQ(blockAt(*engine, 2, 2, 2), nullptr);
  CHECK_EQ(countAt(*engine, 2, 2, 2), 1u);
  // (3,3,3): echo preserved membership, meta applied.
  const auto* anchor = blockAt(*engine, 3, 3, 3);
  CHECK_NE(anchor, nullptr);
  if (anchor) {
    CHECK_EQ(anchor->id, 300u);
    CHECK_EQ(anchor->meta, 7u);
    CHECK_EQ(anchor->mb_id, 55u);
  }
  // (4,4,4): created last, untouched by everything above.
  const auto* last = blockAt(*engine, 4, 4, 4);
  CHECK_NE(last, nullptr);
  if (last) {
    CHECK_EQ(last->id, 400u);
    CHECK_EQ(last->meta, 4u);
    CHECK_EQ(last->mb_id, 0u);
  }
}

// Two independent engines fed the same batch diverge only where the batches
// differ — the handler holds no shared state between instances.
static void test_HandlerInstancesAreIndependent() {
  auto a = std::make_shared<SimulationEngine>();
  auto b = std::make_shared<SimulationEngine>();
  ChunkEventHandler handler_a(a);
  ChunkEventHandler handler_b(b);

  handler_a.handle(makeEvent(1, 1, 1, 10, 0, 0));
  handler_b.handle(makeEvent(1, 1, 1, 10, 0, 0));
  handler_b.handle(makeEvent(2, 2, 2, 20, 0, 0));

  CHECK_EQ(positionCount(*a), 1u);
  CHECK_EQ(positionCount(*b), 2u);
}

} // namespace

int main() {
  TEST(SnapshotReadsCoordFromEventPos);
  TEST(SnapshotCoordIsSigned);
  TEST(SnapshotOwnsACopyOfThePayload);
  TEST(EmptyPayloadYieldsZeroedSnapshot);
  TEST(GettersAlwaysReturnZero);
  TEST(GettersReturnZeroAtCoordinateExtremes);
  TEST(ConstructionIsDeterministic);
  TEST(AnyFlatbufferWithAStructAtSlotFourIsAccepted);
  TEST(GarbageBytesDoNotBecomeACoordinate);
  TEST(EmptyPayloadIsIgnored);
  TEST(InvalidBuffersAreIgnored);
  TEST(RejectionDoesNotPoisonLaterEvents);
  TEST(ValidEventCreatesBlockEntity);
  TEST(HandlerDoesNotMutateTheInput);
  TEST(DistinctPositionsCreateDistinctEntities);
  TEST(SamePositionIsUpdatedInPlace);
  TEST(EchoWithZeroMbIdPreservesExistingMbId);
  TEST(ChangingBlockIdResetsMbId);
  TEST(AirEventClearsBlockButKeepsEntity);
  TEST(AirEventOnUnknownPositionIsNoOp);
  TEST(NegativeCoordinatesWrapToUint32);
  TEST(CorrelationFieldsDoNotAffectWorldState);
  TEST(MixedBatchIsAppliedInOrder);
  TEST(HandlerInstancesAreIndependent);
  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
