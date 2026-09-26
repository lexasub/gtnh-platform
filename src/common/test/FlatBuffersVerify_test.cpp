// Unit tests for gtnh::wire::VerifyAndGetRoot (issue gp-wyvv).
//
// WHY THIS TEST EXISTS
// --------------------
// VerifyAndGetRoot is the single gate every network-facing FlatBuffers parse
// in this repo now goes through, so the property it promises — "you only get a
// Table* when the bytes are a structurally valid FlatBuffer of that root type"
// — needs to be pinned rather than assumed. A regression here would silently
// re-open the out-of-bounds read the gate exists to close, and no other test in
// the tree would notice: the call sites simply hand the bad pointer to a
// handler that would crash somewhere far away.
//
// It also pins the finding that motivates the helper. The call sites this
// replaced looked guarded, because they wrote
//
//     auto* flow = flatbuffers::GetRoot<Protocol::EnergyFlowEvent>(data.data());
//     if (!flow || !flow->pos()) return;
//
// but GetRoot never returns null — it manufactures a Table* from whatever the
// bytes contain. The `!flow` arm was dead code that read as protection. The
// first test below states that outright, because if a future flatbuffers
// release ever DID make GetRoot return null on garbage, the reason this helper
// exists would have to be revisited.
//
// Harness: the project's own CHECK/TEST macros (src/engine/net/test/test.h,
// mirrored by src/game/world/test/BlockTransforms_test.cpp). The repo has zero
// GoogleTest usage and CI does not install it.

#include "core_generated.h"
#include <common/FlatBuffersVerify.h>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <vector>

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

#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)

#define TEST(name) do { printf("  TEST: %s\n", #name); test_##name(); } while (0)

namespace {

using gtnh::wire::VerifyAndGetRoot;

std::vector<uint8_t> validEvent(int32_t x, int32_t y, int32_t z) {
  flatbuffers::FlatBufferBuilder fbb;
  auto pos = Protocol::Vec3i(x, y, z);
  fbb.Finish(Protocol::CreateBlockChangedEvent(fbb, &pos, 7, 1, 0));
  return std::vector<uint8_t>(fbb.GetBufferPointer(),
                              fbb.GetBufferPointer() + fbb.GetSize());
}

// The defect, stated plainly: GetRoot is a reinterpret cast, not a parse. It
// hands back a non-null pointer for bytes that are not a FlatBuffer at all, so
// a `if (!root)` check downstream is unreachable. Everything the helper does is
// only meaningful because of this.
static void test_GetRootAloneDoesNotValidate() {
  std::vector<uint8_t> garbage(64, 0xAB);
  CHECK(flatbuffers::GetRoot<Protocol::BlockChangedEvent>(garbage.data()) != nullptr);

  // A 3-byte buffer is also "parsed" into a non-null Table*.
  std::vector<uint8_t> tiny(3, 0);
  CHECK(flatbuffers::GetRoot<Protocol::BlockChangedEvent>(tiny.data()) != nullptr);

  // ...and so is a non-null pointer paired with a zero length, which is the
  // shape ChunkSnapshot sees when handed an empty sub-range. (An empty
  // std::vector's data() is itself nullptr on this libstdc++, so a real
  // non-null pointer is used here to isolate the length check.)
  std::vector<uint8_t> backing(16, 0xEE);
  CHECK(flatbuffers::GetRoot<Protocol::BlockChangedEvent>(backing.data()) != nullptr);
}

// A buffer we produced ourselves must still parse, and must still decode to
// exactly what was written. Verification that rejects valid traffic is a
// functional regression, not a security fix, so this guards both directions.
static void test_ValidBufferIsAcceptedAndDecodesCorrectly() {
  const auto buf = validEvent(7, 8, 9);
  const auto* ev = VerifyAndGetRoot<Protocol::BlockChangedEvent>(buf.data(), buf.size());

  CHECK(ev != nullptr);
  CHECK(ev && ev->pos() != nullptr);
  CHECK(ev && ev->pos()->x() == 7);
  CHECK(ev && ev->pos()->y() == 8);
  CHECK(ev && ev->pos()->z() == 9);
  CHECK(ev && ev->block_id() == 7);
  CHECK(ev && ev->meta() == 1);
}

// The shapes a peer or a corrupt frame actually produces.
static void test_MalformedBuffersAreRejected() {
  const auto good = validEvent(4, 5, 6);

  std::vector<uint8_t> garbage(64, 0xAB);
  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(garbage.data(), garbage.size()) == nullptr);

  std::vector<uint8_t> zeros(64, 0);
  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(zeros.data(), zeros.size()) == nullptr);

  // A valid buffer chopped by one byte: the trailing vtable read no longer
  // fits, which is exactly the truncation a half-read frame produces.
  std::vector<uint8_t> truncated(good.begin(), good.end() - 1);
  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(truncated.data(), truncated.size()) == nullptr);
}

// Below FLATBUFFERS_MIN_BUFFER_SIZE there is no room for a root offset plus a
// vtable, so there is nothing to parse even when the pointer is valid.
static void test_TooShortBuffersAreRejected() {
  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(nullptr, 0) == nullptr);

  std::vector<uint8_t> backing(16, 0xEE);
  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(backing.data(), 0) == nullptr);

  std::vector<uint8_t> tiny(3, 0);
  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(tiny.data(), 3) == nullptr);
}

// The length argument is load-bearing: every bounds check the Verifier takes is
// taken against it. Same bytes, honest length => rejected. This is why every
// call site passes the measured size and none of them hardcode a ceiling.
static void test_TrueLengthIsWhatDecides() {
  const auto good = validEvent(1, 2, 3);
  std::vector<uint8_t> truncated(good.begin(), good.end() - 1);

  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(truncated.data(), truncated.size()) == nullptr);
  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(good.data(), good.size()) != nullptr);
}

// LIMITATION, pinned on purpose so it is not rediscovered as a bug in the
// helper: the flatbuffers Verifier is structural, not type-tagged. A ChunkData
// table has a field layout that a BlockChangedEvent vtable also satisfies, so a
// valid-but-wrong root type verifies and is accepted. No Verifier call can
// reject it — closing that would need a schema-level type tag or an
// application-level discriminator, neither of which exists in this protocol.
// src/game/world/test/ChunkEvent_test.cpp documents the same quirk from the
// other direction, where nothing verifies at all.
static void test_CompatibleWrongRootTypeIsStillAccepted() {
  flatbuffers::FlatBufferBuilder fbb(64);
  Protocol::Vec3i coord(7, 8, 9);
  const auto block_vec = fbb.CreateVector(std::vector<uint16_t>{11, 22, 33});
  fbb.Finish(Protocol::CreateChunkData(fbb, &coord, block_vec, 0, 0));
  std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                           fbb.GetBufferPointer() + fbb.GetSize());

  CHECK(VerifyAndGetRoot<Protocol::BlockChangedEvent>(buf.data(), buf.size()) != nullptr);
}

}  // namespace

int main() {
  TEST(GetRootAloneDoesNotValidate);
  TEST(ValidBufferIsAcceptedAndDecodesCorrectly);
  TEST(MalformedBuffersAreRejected);
  TEST(TooShortBuffersAreRejected);
  TEST(TrueLengthIsWhatDecides);
  TEST(CompatibleWrongRootTypeIsStillAccepted);
  printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
