// Unit tests for the ECS component headers under src/engine/sim/components/.
//
// Covers all 16 headers in one target (engine_sim_components_test), tracking
// the 16 beads issues gp-2bs, gp-38d, gp-3l3, gp-933, gp-9p6, gp-9pu, gp-aoa,
// gp-cwe, gp-doj, gp-e8p, gp-i8f, gp-io8, gp-kp4, gp-mb9, gp-ug4, gp-w2h.
//
//   gp-2bs  EnergySource            gp-io8  EnergyStorage
//   gp-38d  InventoryContainer      gp-kp4  MultiblockController
//   gp-3l3  Block                   gp-mb9  Position
//   gp-933  EnergyType              gp-ug4  MachineComponent
//   gp-9p6  RecipeProgress          gp-w2h  BatteryBufferComponent
//   gp-9pu  SteamOutputComponent    gp-aoa  SideConfig
//   gp-cwe  FluidStorage            gp-doj  BiomeComponent
//   gp-e8p  HeatIntakeComponent     gp-i8f  HatchSlot
//
// Every header here is header-only (no .cpp in components/), so this target
// needs no gtnh_engine_sim link and therefore no per-subdir find_package for
// the PRIVATE yaml-cpp dependency it drags in. The only libs pulled in are the
// inline/constexpr pieces the headers include: <engine/registry/ItemId.h>
// (via PatternLibrary.h) and <common/ResourcePort.h> (via RecipeProgress.h's
// __has_include probe). Neither defines an out-of-line symbol we call.
//
// These tests assert the OBSERVED behaviour of the headers, quirks included.
// Nothing here is aspirational: where a helper misbehaves, the test pins the
// actual result and the deviation is reported rather than papered over.
//
// Conventions: project CHECK/TEST harness (src/engine/net/test/test.h), no
// GoogleTest — gtest is absent from conanfile.txt, CI does not install it, and
// a find_package(GTest QUIET) guard would make the test silently vanish from
// CI instead of failing it. No display, no network, no cluster, no clock.

#include <engine/net/test/test.h>

#include <engine/sim/components/BatteryBufferComponent.h>
#include <engine/sim/components/BiomeComponent.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/EnergySource.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/EnergyType.h>
#include <engine/sim/components/FluidStorage.h>
#include <engine/sim/components/HatchSlot.h>
#include <engine/sim/components/HeatIntakeComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/MultiblockController.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/RecipeProgress.h>
#include <engine/sim/components/SideConfig.h>
#include <engine/sim/components/SteamOutputComponent.h>

#include <common/ResourcePort.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// Project unit-test harness. Same shape as src/engine/net/test/test.h's
// per-test definitions and src/game/world/test/BlockTransforms_test.cpp.
// ---------------------------------------------------------------------------
int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr,
                const char* msg) {
  ++g_tests;
  if (cond) {
    ++g_passed;
  } else {
    ++g_failed;
    std::fprintf(stderr, "  FAIL %s:%d: %s%s%s\n", file, line, expr,
                 msg ? " -- " : "", msg ? msg : "");
  }
}

#define CHECK(cond, ...) test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)
#define CHECK_EQ(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#define CHECK_NE(a, b, ...) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, ##__VA_ARGS__)
#define CHECK_GE(a, b, ...) test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, ##__VA_ARGS__)
// Tolerance comparison for the ratio() helpers, which return float/double.
#define CHECK_NEAR(a, b, tol, ...)                                                \
  test_check((((a) - (b)) <= (tol)) && (((b) - (a)) <= (tol)), __FILE__, __LINE__, \
             #a " ~= " #b, ##__VA_ARGS__)

#define TEST(name) do { std::printf("  TEST: %s\n", #name); test_##name(); } while (0)

namespace {

using simcore::BatteryBufferComponent;
using simcore::BiomeComponent;
using simcore::Block;
using simcore::EnergySource;
using simcore::EnergyStorage;
using simcore::EnergyType;
using simcore::FluidStorage;
using simcore::HatchSlot;
using simcore::HeatIntakeComponent;
using simcore::InventoryContainer;
using simcore::InventorySlot;
using simcore::MachineComponent;
using simcore::MachineFaceRole;
using simcore::MultiblockController;
using simcore::PendingCraft;
using simcore::Position;
using simcore::RecipeProgress;
using simcore::RequirementReservation;
using simcore::ResourceKind;
using simcore::ResourceReservation;
using simcore::SideRole;
using simcore::SteamOutputComponent;

using HatchType = simcore::HatchType;

// ===========================================================================
// gp-933 — EnergyType.h
// ===========================================================================

static void test_EnergyType_declared_ordinals() {
  CHECK_EQ(static_cast<uint8_t>(EnergyType::ELECTRICITY), 0u);
  CHECK_EQ(static_cast<uint8_t>(EnergyType::HEAT), 1u);
  CHECK_EQ(static_cast<uint8_t>(EnergyType::STEAM), 2u);
  CHECK_EQ(static_cast<uint8_t>(EnergyType::ROTATION), 3u);
}

static void test_EnergyType_is_uint8_backed_scoped_enum() {
  CHECK((std::is_enum<EnergyType>::value));
  CHECK((std::is_same<std::underlying_type<EnergyType>::type, uint8_t>::value));
  CHECK_EQ(sizeof(EnergyType), sizeof(uint8_t));
  // Scoped: the enumerators are not injected into the enclosing namespace.
  CHECK((!std::is_convertible<EnergyType, int>::value));
}

// ===========================================================================
// gp-2bs — EnergySource.h
// ===========================================================================

static void test_EnergySource_is_an_empty_tag() {
  CHECK((std::is_empty<EnergySource>::value));
  // An empty class still occupies one byte, but stores nothing.
  CHECK_EQ(sizeof(EnergySource), static_cast<size_t>(1));
}

static void test_EnergySource_is_trivially_default_constructible() {
  CHECK((std::is_trivially_default_constructible<EnergySource>::value));
  CHECK((std::is_trivially_copyable<EnergySource>::value));
  CHECK((std::is_standard_layout<EnergySource>::value));
  // Value-initialising the tag is legal and yields no state at all.
  const EnergySource source{};
  (void)source;
  CHECK(true);
}

// ===========================================================================
// gp-3l3 — Block.h
// ===========================================================================

static void test_Block_defaults_are_zero() {
  const Block block;
  CHECK_EQ(block.id, static_cast<uint16_t>(0));
  CHECK_EQ(block.meta, static_cast<uint8_t>(0));
  CHECK_EQ(block.mb_id, static_cast<uint32_t>(0));
}

static void test_Block_full_constructor_stores_all_fields() {
  const Block block(static_cast<uint16_t>(0x1234), static_cast<uint8_t>(5),
                    static_cast<uint32_t>(0xDEADBEEF));
  CHECK_EQ(block.id, static_cast<uint16_t>(0x1234));
  CHECK_EQ(block.meta, static_cast<uint8_t>(5));
  CHECK_EQ(block.mb_id, static_cast<uint32_t>(0xDEADBEEF));
}

// OBSERVED: mb_id 0 is the documented "no multiblock" sentinel, and the type
// has no way to distinguish "unset" from "explicitly 0" — the value is the
// only signal, exactly as ChunkStore's block_id/meta/mb_id triple assumes.
static void test_Block_zero_mb_id_is_the_single_block_sentinel() {
  const Block single(static_cast<uint16_t>(100), static_cast<uint8_t>(0), 0u);
  const Block multi(static_cast<uint16_t>(100), static_cast<uint8_t>(0), 1u);
  CHECK_EQ(single.mb_id, static_cast<uint32_t>(0));
  CHECK_NE(single.mb_id, multi.mb_id);
  CHECK_EQ(single.id, multi.id);
  CHECK_EQ(single.meta, multi.meta);
}

static void test_Block_aggregate_initialisation_order_is_id_meta_mb() {
  const Block block{static_cast<uint16_t>(7), static_cast<uint8_t>(3),
                    static_cast<uint32_t>(9)};
  CHECK_EQ(block.id, static_cast<uint16_t>(7));
  CHECK_EQ(block.meta, static_cast<uint8_t>(3));
  CHECK_EQ(block.mb_id, static_cast<uint32_t>(9));
}

// ===========================================================================
// gp-doj — BiomeComponent.h
// ===========================================================================

static void test_Biome_explicit_constructor_stores_id() {
  const BiomeComponent biome(static_cast<uint16_t>(42));
  CHECK_EQ(biome.biome_id, static_cast<uint16_t>(42));
}

static void test_Biome_accepts_the_full_uint16_range() {
  const BiomeComponent low(0);
  const BiomeComponent high(65535);
  CHECK_EQ(low.biome_id, static_cast<uint16_t>(0));
  CHECK_EQ(high.biome_id, static_cast<uint16_t>(65535));
}

// OBSERVED: biome_id has no default member initialiser, so plain
// `BiomeComponent b;` is default-initialised and biome_id is indeterminate.
// Only the value-initialised form is guaranteed zero. Pinned here so the
// difference is documented rather than discovered.
static void test_Biome_value_initialisation_zeroes_id() {
  const BiomeComponent biome{};
  CHECK_EQ(biome.biome_id, static_cast<uint16_t>(0));
}

// ===========================================================================
// gp-mb9 — Position.h
// ===========================================================================

static void test_Position_defaults_are_origin() {
  const Position pos;
  CHECK_EQ(pos.x, static_cast<uint32_t>(0));
  CHECK_EQ(pos.y, static_cast<uint32_t>(0));
  CHECK_EQ(pos.z, static_cast<uint32_t>(0));
}

static void test_Position_constructor_stores_coordinates() {
  const Position pos(10u, 64u, 0xFFFFFFFFu);
  CHECK_EQ(pos.x, static_cast<uint32_t>(10));
  CHECK_EQ(pos.y, static_cast<uint32_t>(64));
  CHECK_EQ(pos.z, static_cast<uint32_t>(0xFFFFFFFF));
}

static void test_Position_is_unsigned_so_negative_coords_wrap() {
  // World coordinates are unsigned in this component; a negative offset wraps
  // rather than clamping. Downstream chunk math must guard before subtracting.
  const Position pos(0u, 0u, 0u);
  const uint32_t wrapped = pos.z - 1u;
  CHECK_EQ(wrapped, static_cast<uint32_t>(0xFFFFFFFF));
}

// ===========================================================================
// gp-w2h — BatteryBufferComponent.h
// ===========================================================================

static void test_BatteryBuffer_is_zero_initialised_when_aggregated() {
  const BatteryBufferComponent buffer{};
  CHECK_EQ(buffer.capacity, static_cast<uint32_t>(0));
  CHECK_EQ(buffer.stored, static_cast<int32_t>(0));
  CHECK_EQ(buffer.tier, static_cast<uint8_t>(0));
  CHECK_EQ(buffer.maxInput, static_cast<int32_t>(0));
  CHECK_EQ(buffer.chargeRate, static_cast<int32_t>(0));
  CHECK_EQ(buffer.numSlots, static_cast<uint8_t>(0));
}

static void test_BatteryBuffer_holds_the_documented_GTNH_tier_values() {
  // Comments in the header pin the design values; they are documentation only
  // (the struct has no helpers), so this asserts the field types can carry
  // them without loss, not that the struct computes them.
  const BatteryBufferComponent lv{40000u, 0, 1u, 32, 8, 1u};
  CHECK_EQ(lv.capacity, static_cast<uint32_t>(40000));
  CHECK_EQ(lv.maxInput, static_cast<int32_t>(32));
  CHECK_EQ(lv.chargeRate, static_cast<int32_t>(8));
  CHECK_EQ(lv.numSlots, static_cast<uint8_t>(1));

  const BatteryBufferComponent mv{150000u, 0, 2u, 128, 32, 2u};
  CHECK_EQ(mv.capacity, static_cast<uint32_t>(150000));
  CHECK_EQ(mv.maxInput, static_cast<int32_t>(128));
  CHECK_EQ(mv.numSlots, static_cast<uint8_t>(2));

  const BatteryBufferComponent hv{600000u, 0, 3u, 512, 128, 4u};
  CHECK_EQ(hv.capacity, static_cast<uint32_t>(600000));
  CHECK_EQ(hv.maxInput, static_cast<int32_t>(512));
  CHECK_EQ(hv.numSlots, static_cast<uint8_t>(4));
}

static void test_BatteryBuffer_tier_and_slots_are_narrow_types() {
  // tier 0=ULV .. 3=HV, and numSlots maxes at 4 for HV: both fit in uint8_t.
  const BatteryBufferComponent buffer{600000u, 0, 255u, 512, 128, 255u};
  CHECK_EQ(sizeof(buffer.tier), static_cast<size_t>(1));
  CHECK_EQ(sizeof(buffer.numSlots), static_cast<size_t>(1));
  CHECK_EQ(sizeof(buffer.stored), static_cast<size_t>(4));
  CHECK_EQ(sizeof(buffer.capacity), static_cast<size_t>(4));
  CHECK_EQ(buffer.tier, static_cast<uint8_t>(255));
  CHECK_EQ(buffer.numSlots, static_cast<uint8_t>(255));
}

static void test_BatteryBuffer_stores_energy_and_acceptance_independently() {
  // stored is the only mutable state; the rest is per-tier configuration.
  BatteryBufferComponent buffer{40000u, 0, 1u, 32, 8, 1u};
  buffer.stored = 40000;
  CHECK_EQ(buffer.stored, static_cast<int32_t>(40000));
  CHECK_EQ(buffer.capacity, static_cast<uint32_t>(40000));
}

// ===========================================================================
// gp-io8 — EnergyStorage.h
// ===========================================================================

static void test_EnergyStorage_default_constructor_is_all_zero_ELECTRICITY() {
  const EnergyStorage storage;
  CHECK_EQ(storage.capacity, static_cast<int32_t>(0));
  CHECK_EQ(storage.current, static_cast<int32_t>(0));
  CHECK_EQ(storage.maxInput, static_cast<int32_t>(0));
  CHECK_EQ(storage.maxOutput, static_cast<int32_t>(0));
  CHECK_EQ(storage.tier, static_cast<int32_t>(0));
  CHECK_EQ(static_cast<uint8_t>(storage.type), static_cast<uint8_t>(EnergyType::ELECTRICITY));
}

static void test_EnergyStorage_full_constructor_defaults_type_to_ELECTRICITY() {
  const EnergyStorage storage(1000, 250, 64, 32, 2);
  CHECK_EQ(storage.capacity, static_cast<int32_t>(1000));
  CHECK_EQ(storage.current, static_cast<int32_t>(250));
  CHECK_EQ(storage.maxInput, static_cast<int32_t>(64));
  CHECK_EQ(storage.maxOutput, static_cast<int32_t>(32));
  CHECK_EQ(storage.tier, static_cast<int32_t>(2));
  CHECK_EQ(static_cast<uint8_t>(storage.type), static_cast<uint8_t>(EnergyType::ELECTRICITY));
}

static void test_EnergyStorage_full_constructor_with_type_sets_type() {
  const EnergyStorage storage(1000, 250, 64, 32, 2, EnergyType::HEAT);
  CHECK_EQ(static_cast<uint8_t>(storage.type), static_cast<uint8_t>(EnergyType::HEAT));

  const EnergyStorage steam(1000, 0, 64, 32, 0, EnergyType::STEAM);
  CHECK_EQ(static_cast<uint8_t>(steam.type), static_cast<uint8_t>(EnergyType::STEAM));
}

static void test_EnergyStorage_addEnergy_takes_whole_request_below_limits() {
  // maxInput (128) admits the whole 100-unit request, and 1000 of capacity
  // leaves room, so nothing is clamped.
  EnergyStorage storage(1000, 0, 128, 0, 0);
  const int32_t accepted = storage.addEnergy(100);
  CHECK_EQ(accepted, static_cast<int32_t>(100));
  CHECK_EQ(storage.current, static_cast<int32_t>(100));
}

static void test_EnergyStorage_addEnergy_clamps_to_maxInput() {
  // maxInput gates EXTERNAL energy per tick, independent of free space.
  EnergyStorage storage(1000, 0, 64, 0, 0);
  const int32_t accepted = storage.addEnergy(100);
  CHECK_EQ(accepted, static_cast<int32_t>(64));
  CHECK_EQ(storage.current, static_cast<int32_t>(64));

  // A second tick is clamped by maxInput again, NOT by the remaining space:
  // the per-tick limit is a rate, not a one-off.
  CHECK_EQ(storage.addEnergy(100), static_cast<int32_t>(64));
  CHECK_EQ(storage.current, static_cast<int32_t>(128));
}

static void test_EnergyStorage_addEnergy_clamps_to_remaining_space() {
  // maxInput is generous, but only 10 units of headroom exist.
  EnergyStorage storage(1000, 990, 500, 0, 0);
  const int32_t accepted = storage.addEnergy(100);
  CHECK_EQ(accepted, static_cast<int32_t>(10));
  CHECK_EQ(storage.current, static_cast<int32_t>(1000));
}

static void test_EnergyStorage_addEnergy_takes_the_tighter_of_space_and_maxInput() {
  EnergyStorage storage(1000, 995, 64, 0, 0);  // 5 free, 64 per-tick limit
  CHECK_EQ(storage.addEnergy(100), static_cast<int32_t>(5));
  CHECK_EQ(storage.current, static_cast<int32_t>(1000));
}

static void test_EnergyStorage_addEnergy_accepts_nothing_when_full() {
  EnergyStorage storage(1000, 1000, 64, 0, 0);
  CHECK_EQ(storage.addEnergy(1), static_cast<int32_t>(0));
  CHECK_EQ(storage.current, static_cast<int32_t>(1000));
}

static void test_EnergyStorage_addEnergy_with_zero_maxInput_accepts_nothing() {
  // A default-constructed (or generator) storage has maxInput 0, so external
  // input is rejected entirely even with 1000 units free.
  EnergyStorage storage(1000, 0, 0, 0, 0);
  CHECK_EQ(storage.addEnergy(500), static_cast<int32_t>(0));
  CHECK_EQ(storage.current, static_cast<int32_t>(0));
}

static void test_EnergyStorage_addEnergy_ignores_non_positive_requests() {
  EnergyStorage storage(1000, 100, 64, 0, 0);
  CHECK_EQ(storage.addEnergy(0), static_cast<int32_t>(0));
  // addEnergy DOES guard negatives, unlike consumeEnergy.
  CHECK_EQ(storage.addEnergy(-50), static_cast<int32_t>(0));
  CHECK_EQ(storage.current, static_cast<int32_t>(100));
}

// OBSERVED DEVIATION: addEnergy's final clamp is an unguarded
// `if (accepted > maxInput) accepted = maxInput;`. A negative maxInput is
// therefore not rejected — it inverts the operation: current moves backwards
// and a negative amount is reported as "accepted". Only a positive maxInput
// is safe to rely on.
static void test_EnergyStorage_addEnergy_with_negative_maxInput_moves_energy_backwards() {
  EnergyStorage storage(1000, 100, -1, 0, 0);
  const int32_t accepted = storage.addEnergy(50);
  CHECK_EQ(accepted, static_cast<int32_t>(-1));
  CHECK_EQ(storage.current, static_cast<int32_t>(99));
}

static void test_EnergyStorage_produceEnergy_bypasses_maxInput() {
  // Documented intent: self-production is limited only by capacity, because
  // maxInput governs EXTERNAL energy.
  EnergyStorage generator(1000, 0, 0, 0, 0);
  const int32_t produced = generator.produceEnergy(500);
  CHECK_EQ(produced, static_cast<int32_t>(500));
  CHECK_EQ(generator.current, static_cast<int32_t>(500));
}

static void test_EnergyStorage_produceEnergy_clamps_to_remaining_space() {
  EnergyStorage generator(1000, 900, 0, 0, 0);
  CHECK_EQ(generator.produceEnergy(500), static_cast<int32_t>(100));
  CHECK_EQ(generator.current, static_cast<int32_t>(1000));
}

static void test_EnergyStorage_produceEnergy_on_full_buffer_accepts_nothing() {
  EnergyStorage generator(1000, 1000, 0, 0, 0);
  CHECK_EQ(generator.produceEnergy(1), static_cast<int32_t>(0));
  CHECK_EQ(generator.current, static_cast<int32_t>(1000));
}

static void test_EnergyStorage_produceEnergy_ignores_non_positive_requests() {
  EnergyStorage generator(1000, 500, 0, 0, 0);
  CHECK_EQ(generator.produceEnergy(0), static_cast<int32_t>(0));
  CHECK_EQ(generator.produceEnergy(-100), static_cast<int32_t>(0));
  CHECK_EQ(generator.current, static_cast<int32_t>(500));
}

static void test_EnergyStorage_produceEnergy_survives_over_capacity_state() {
  // current above capacity yields negative headroom, which is clamped to 0
  // rather than wrapped.
  EnergyStorage generator(1000, 1200, 0, 0, 0);
  CHECK_EQ(generator.produceEnergy(100), static_cast<int32_t>(0));
  CHECK_EQ(generator.current, static_cast<int32_t>(1200));
}

static void test_EnergyStorage_consumeEnergy_takes_whole_request_below_limits() {
  EnergyStorage storage(1000, 500, 64, 128, 0);
  const int32_t consumed = storage.consumeEnergy(100);
  CHECK_EQ(consumed, static_cast<int32_t>(100));
  CHECK_EQ(storage.current, static_cast<int32_t>(400));
}

static void test_EnergyStorage_consumeEnergy_clamps_to_available() {
  EnergyStorage storage(1000, 30, 64, 128, 0);
  const int32_t consumed = storage.consumeEnergy(100);
  CHECK_EQ(consumed, static_cast<int32_t>(30));
  CHECK_EQ(storage.current, static_cast<int32_t>(0));
}

static void test_EnergyStorage_consumeEnergy_clamps_to_maxOutput() {
  EnergyStorage storage(1000, 500, 64, 32, 0);
  CHECK_EQ(storage.consumeEnergy(100), static_cast<int32_t>(32));
  CHECK_EQ(storage.current, static_cast<int32_t>(468));
}

static void test_EnergyStorage_consumeEnergy_with_zero_maxOutput_consumes_nothing() {
  EnergyStorage storage(1000, 500, 64, 0, 0);
  CHECK_EQ(storage.consumeEnergy(100), static_cast<int32_t>(0));
  CHECK_EQ(storage.current, static_cast<int32_t>(500));
}

static void test_EnergyStorage_consumeEnergy_ignores_zero_requests() {
  EnergyStorage storage(1000, 500, 64, 128, 0);
  CHECK_EQ(storage.consumeEnergy(0), static_cast<int32_t>(0));
  CHECK_EQ(storage.current, static_cast<int32_t>(500));
}

// OBSERVED DEVIATION: unlike addEnergy, consumeEnergy has no
// `if (available < 0)` guard, so a NEGATIVE request is not rejected —
// `available = amount` and `current -= available` CREDIT energy back, and the
// negative count is reported as "consumed". Contrast with
// addEnergy, whose explicit `if (accepted < 0) accepted = 0;` does reject it.
static void test_EnergyStorage_consumeEnergy_with_a_negative_request_credits_energy_back() {
  EnergyStorage storage(1000, 500, 64, 128, 0);
  const int32_t consumed = storage.consumeEnergy(-25);
  CHECK_EQ(consumed, static_cast<int32_t>(-25));
  CHECK_EQ(storage.current, static_cast<int32_t>(525));
}

static void test_EnergyStorage_consumeEnergy_can_overshoot_into_negative_current() {
  // No lower clamp: requesting more than maxOutput on an empty buffer reports
  // 0 consumed and leaves current at 0, but a request against a partially
  // filled buffer can drive current below 0.
  EnergyStorage storage(1000, 10, 64, 128, 0);
  CHECK_EQ(storage.consumeEnergy(50), static_cast<int32_t>(10));
  CHECK_EQ(storage.current, static_cast<int32_t>(0));

  // Negative current then makes isEmpty() true and addEnergy() headroom huge.
  storage.current = -5;
  CHECK(storage.isEmpty());
  CHECK_EQ(storage.addEnergy(3), static_cast<int32_t>(3));
  CHECK_EQ(storage.current, static_cast<int32_t>(-2));
}

// OBSERVED DEVIATION: consumeEnergy has no `if (available < 0)` guard, so a
// negative current makes `available` negative and `current -= available`
// ADDS energy back, returning a negative "consumed" count.
static void test_EnergyStorage_consumeEnergy_on_negative_current_credits_energy_back() {
  EnergyStorage storage(1000, -10, 64, 128, 0);
  const int32_t consumed = storage.consumeEnergy(5);
  CHECK_EQ(consumed, static_cast<int32_t>(-10));
  CHECK_EQ(storage.current, static_cast<int32_t>(0));
}

// OBSERVED DEVIATION: a negative maxOutput inverts consumeEnergy the same way
// addEnergy's negative maxInput does.
static void test_EnergyStorage_consumeEnergy_with_negative_maxOutput_credits_energy_back() {
  EnergyStorage storage(1000, 100, 64, -1, 0);
  const int32_t consumed = storage.consumeEnergy(50);
  CHECK_EQ(consumed, static_cast<int32_t>(-1));
  CHECK_EQ(storage.current, static_cast<int32_t>(101));
}

static void test_EnergyStorage_isFull_and_isEmpty_boundaries() {
  EnergyStorage storage(1000, 0, 0, 0, 0);
  CHECK(storage.isEmpty());
  CHECK(!storage.isFull());

  storage.current = 1;
  CHECK(!storage.isEmpty());
  CHECK(!storage.isFull());

  storage.current = 999;
  CHECK(!storage.isFull());
  CHECK(!storage.isEmpty());

  storage.current = 1000;
  CHECK(storage.isFull());
  CHECK(!storage.isEmpty());

  // Observed: both predicates are >= / <=, so an over-full buffer is full
  // while a negative current is empty.
  storage.current = 5000;
  CHECK(storage.isFull());
  storage.current = -1;
  CHECK(storage.isEmpty());
}

static void test_EnergyStorage_default_constructed_is_simultaneously_empty_and_full() {
  // capacity 0 == current 0, so both predicates are true. Any system that
  // asks "is it empty?" before "is it full?" gets both answers.
  EnergyStorage storage;
  CHECK(storage.isEmpty());
  CHECK(storage.isFull());
  CHECK_EQ(storage.addEnergy(100), static_cast<int32_t>(0));
  CHECK_EQ(storage.produceEnergy(100), static_cast<int32_t>(0));
  CHECK_EQ(storage.consumeEnergy(100), static_cast<int32_t>(0));
}

// OBSERVED DEVIATION: addEnergy/produceEnergy/consumeEnergy are NOT const
// members, so a `const EnergyStorage` cannot be queried for a hypothetical
// fill without a const_cast. isFull()/isEmpty() are const. Same asymmetry in
// FluidStorage (addFluid/removeFluid non-const) and InventoryContainer
// (setSlot/addItem/removeItem non-const, getSlot const). Pinned via
// decltype so a future const-qualification has to update this test on purpose.
static void test_EnergyStorage_mutators_are_not_const_members() {
  CHECK((std::is_same<decltype(&EnergyStorage::addEnergy),
                      int32_t (EnergyStorage::*)(int32_t)>::value));
  CHECK((std::is_same<decltype(&EnergyStorage::produceEnergy),
                      int32_t (EnergyStorage::*)(int32_t)>::value));
  CHECK((std::is_same<decltype(&EnergyStorage::consumeEnergy),
                      int32_t (EnergyStorage::*)(int32_t)>::value));
  CHECK((std::is_same<decltype(&EnergyStorage::isFull),
                      bool (EnergyStorage::*)() const>::value));
  CHECK((std::is_same<decltype(&EnergyStorage::isEmpty),
                      bool (EnergyStorage::*)() const>::value));
}

static void test_FluidStorage_mutators_are_not_const_members() {
  CHECK((std::is_same<decltype(&FluidStorage::addFluid),
                      int32_t (FluidStorage::*)(int32_t)>::value));
  CHECK((std::is_same<decltype(&FluidStorage::removeFluid),
                      int32_t (FluidStorage::*)(int32_t)>::value));
  CHECK((std::is_same<decltype(&FluidStorage::isFull),
                      bool (FluidStorage::*)() const>::value));
  CHECK((std::is_same<decltype(&FluidStorage::isEmpty),
                      bool (FluidStorage::*)() const>::value));
}

static void test_InventoryContainer_mutation_helpers_are_not_const_members() {
  CHECK((std::is_same<decltype(&InventoryContainer::getSlot),
                      InventorySlot (InventoryContainer::*)(uint16_t) const>::value));
  CHECK((std::is_same<decltype(&InventoryContainer::setSlot),
                      void (InventoryContainer::*)(uint16_t, const InventorySlot &)>::value));
  CHECK((std::is_same<decltype(&InventoryContainer::addItem),
                      int32_t (InventoryContainer::*)(uint16_t, uint8_t, uint16_t)>::value));
  CHECK((std::is_same<decltype(&InventoryContainer::removeItem),
                      bool (InventoryContainer::*)(uint16_t, uint8_t)>::value));
}

static void test_EnergyStorage_charge_then_discharge_round_trips() {
  EnergyStorage storage(1000, 0, 64, 32, 1);
  const int32_t charged = storage.addEnergy(1000);
  CHECK_EQ(charged, static_cast<int32_t>(64));
  for (int tick = 0; tick < 15; ++tick) storage.addEnergy(64);
  CHECK_EQ(storage.current, static_cast<int32_t>(1000));
  CHECK(storage.isFull());

  const int32_t drained = storage.consumeEnergy(1000);
  CHECK_EQ(drained, static_cast<int32_t>(32));
  for (int tick = 0; tick < 31; ++tick) storage.consumeEnergy(32);
  CHECK_EQ(storage.current, static_cast<int32_t>(0));
  CHECK(storage.isEmpty());
}

// ===========================================================================
// gp-cwe — FluidStorage.h
// ===========================================================================

static void test_FluidStorage_defaults_are_all_zero() {
  const FluidStorage fluid;
  CHECK_EQ(fluid.fluid_id, static_cast<uint32_t>(0));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(0));
  CHECK_EQ(fluid.capacity, static_cast<int32_t>(0));
  CHECK_EQ(fluid.maxInput, static_cast<int32_t>(0));
  CHECK_EQ(fluid.maxOutput, static_cast<int32_t>(0));
}

static void test_FluidStorage_full_constructor_stores_all_fields() {
  const FluidStorage fluid(42u, 500, 1000, 100, 200);
  CHECK_EQ(fluid.fluid_id, static_cast<uint32_t>(42));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(500));
  CHECK_EQ(fluid.capacity, static_cast<int32_t>(1000));
  CHECK_EQ(fluid.maxInput, static_cast<int32_t>(100));
  CHECK_EQ(fluid.maxOutput, static_cast<int32_t>(200));
}

static void test_FluidStorage_isFull_and_isEmpty_boundaries() {
  FluidStorage fluid(1u, 0, 1000, 0, 0);
  CHECK(fluid.isEmpty());
  CHECK(!fluid.isFull());

  fluid.amount = 1;
  CHECK(!fluid.isEmpty());
  CHECK(!fluid.isFull());

  fluid.amount = 999;
  CHECK(!fluid.isFull());

  fluid.amount = 1000;
  CHECK(fluid.isFull());

  fluid.amount = 2000;
  CHECK(fluid.isFull());
  fluid.amount = -1;
  CHECK(fluid.isEmpty());
}

static void test_FluidStorage_default_constructed_is_empty_and_full() {
  FluidStorage fluid;
  CHECK(fluid.isEmpty());
  CHECK(fluid.isFull());
  CHECK_EQ(fluid.addFluid(100), static_cast<int32_t>(0));
  CHECK_EQ(fluid.removeFluid(100), static_cast<int32_t>(0));
}

static void test_FluidStorage_addFluid_takes_whole_request_below_limits() {
  FluidStorage fluid(1u, 0, 1000, 100, 100);
  CHECK_EQ(fluid.addFluid(50), static_cast<int32_t>(50));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(50));
}

static void test_FluidStorage_addFluid_clamps_to_maxInput() {
  FluidStorage fluid(1u, 0, 1000, 25, 100);
  CHECK_EQ(fluid.addFluid(100), static_cast<int32_t>(25));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(25));
  CHECK_EQ(fluid.addFluid(100), static_cast<int32_t>(25));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(50));
}

static void test_FluidStorage_addFluid_clamps_to_remaining_space() {
  FluidStorage fluid(1u, 990, 1000, 1000, 1000);
  CHECK_EQ(fluid.addFluid(100), static_cast<int32_t>(10));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(1000));
}

static void test_FluidStorage_addFluid_accepts_nothing_when_full() {
  FluidStorage fluid(1u, 1000, 1000, 100, 100);
  CHECK_EQ(fluid.addFluid(1), static_cast<int32_t>(0));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(1000));
}

static void test_FluidStorage_addFluid_with_zero_maxInput_accepts_nothing() {
  FluidStorage fluid(1u, 0, 1000, 0, 0);
  CHECK_EQ(fluid.addFluid(500), static_cast<int32_t>(0));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(0));
}

static void test_FluidStorage_addFluid_ignores_non_positive_requests() {
  FluidStorage fluid(1u, 100, 1000, 100, 100);
  CHECK_EQ(fluid.addFluid(0), static_cast<int32_t>(0));
  // addFluid DOES guard negatives (accepted < 0 -> 0), unlike removeFluid.
  CHECK_EQ(fluid.addFluid(-10), static_cast<int32_t>(0));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(100));
}

// Same unguarded maxInput clamp as EnergyStorage::addEnergy: a negative
// maxInput inverts addFluid.
static void test_FluidStorage_addFluid_with_negative_maxInput_moves_fluid_backwards() {
  FluidStorage fluid(1u, 100, 1000, -1, 100);
  CHECK_EQ(fluid.addFluid(50), static_cast<int32_t>(-1));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(99));
}

static void test_FluidStorage_removeFluid_takes_whole_request_below_limits() {
  FluidStorage fluid(1u, 500, 1000, 100, 100);
  CHECK_EQ(fluid.removeFluid(100), static_cast<int32_t>(100));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(400));
}

static void test_FluidStorage_removeFluid_clamps_to_available() {
  FluidStorage fluid(1u, 30, 1000, 100, 1000);
  CHECK_EQ(fluid.removeFluid(100), static_cast<int32_t>(30));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(0));
}

static void test_FluidStorage_removeFluid_clamps_to_maxOutput() {
  FluidStorage fluid(1u, 500, 1000, 100, 40);
  CHECK_EQ(fluid.removeFluid(100), static_cast<int32_t>(40));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(460));
}

static void test_FluidStorage_removeFluid_with_zero_maxOutput_removes_nothing() {
  FluidStorage fluid(1u, 500, 1000, 100, 0);
  CHECK_EQ(fluid.removeFluid(100), static_cast<int32_t>(0));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(500));
}

static void test_FluidStorage_removeFluid_ignores_zero_requests() {
  FluidStorage fluid(1u, 500, 1000, 100, 100);
  CHECK_EQ(fluid.removeFluid(0), static_cast<int32_t>(0));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(500));
}

// OBSERVED DEVIATION: removeFluid has no non-positive guard either, so a
// negative request ADDS fluid. Note addFluid DOES guard (`if (accepted < 0)
// accepted = 0;`), so the two directions of the same struct disagree.
static void test_FluidStorage_removeFluid_with_a_negative_request_credits_fluid_back() {
  FluidStorage fluid(1u, 500, 1000, 100, 100);
  const int32_t removed = fluid.removeFluid(-10);
  CHECK_EQ(removed, static_cast<int32_t>(-10));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(510));
}

// Same unguarded maxOutput clamp as EnergyStorage::consumeEnergy.
static void test_FluidStorage_removeFluid_with_negative_maxOutput_credits_fluid_back() {
  FluidStorage fluid(1u, 100, 1000, 100, -1);
  CHECK_EQ(fluid.removeFluid(50), static_cast<int32_t>(-1));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(101));
}

static void test_FluidStorage_fill_and_drain_round_trips() {
  FluidStorage fluid(7u, 0, 1000, 100, 50);
  while (!fluid.isFull()) fluid.addFluid(100);
  CHECK_EQ(fluid.amount, static_cast<int32_t>(1000));
  CHECK_EQ(fluid.fluid_id, static_cast<uint32_t>(7));

  int32_t drained = 0;
  while (!fluid.isEmpty()) drained += fluid.removeFluid(50);
  CHECK_EQ(drained, static_cast<int32_t>(1000));
  CHECK_EQ(fluid.amount, static_cast<int32_t>(0));
}

// OBSERVED: addFluid/removeFluid ignore fluid_id entirely, so a caller can mix
// fluids through one storage. The fluid_id field is metadata only.
static void test_FluidStorage_helpers_ignore_the_fluid_id() {
  FluidStorage fluid(1u, 0, 1000, 1000, 1000);
  fluid.addFluid(100);
  fluid.fluid_id = 2u;  // change the fluid mid-stream
  fluid.addFluid(100);
  CHECK_EQ(fluid.amount, static_cast<int32_t>(200));
  CHECK_EQ(fluid.fluid_id, static_cast<uint32_t>(2));
}

// ===========================================================================
// gp-aoa — SideConfig.h
// ===========================================================================

static void test_SideRole_declared_ordinals() {
  CHECK_EQ(static_cast<uint8_t>(SideRole::INPUT), 0u);
  CHECK_EQ(static_cast<uint8_t>(SideRole::OUTPUT), 1u);
  CHECK_EQ(static_cast<uint8_t>(SideRole::ENERGY), 2u);
  CHECK_EQ(static_cast<uint8_t>(SideRole::FLUID_IN), 3u);
  CHECK_EQ(static_cast<uint8_t>(SideRole::FLUID_OUT), 4u);
  CHECK_EQ(static_cast<uint8_t>(SideRole::ANY), 5u);
  CHECK_EQ(static_cast<uint8_t>(SideRole::NONE), 6u);
}

static void test_DEFAULT_SIDE_CONFIG_is_all_ANY() {
  CHECK_EQ(simcore::DEFAULT_SIDE_CONFIG[0], static_cast<uint8_t>(5));
  CHECK_EQ(simcore::DEFAULT_SIDE_CONFIG[1], static_cast<uint8_t>(5));
  CHECK_EQ(simcore::DEFAULT_SIDE_CONFIG[2], static_cast<uint8_t>(5));
  CHECK_EQ(simcore::DEFAULT_SIDE_CONFIG[3], static_cast<uint8_t>(5));
  CHECK_EQ(simcore::DEFAULT_SIDE_CONFIG[4], static_cast<uint8_t>(5));
  CHECK_EQ(simcore::DEFAULT_SIDE_CONFIG[5], static_cast<uint8_t>(5));
}

static void test_sideRoleName_covers_every_declared_role() {
  CHECK_EQ(std::string(simcore::sideRoleName(SideRole::INPUT)), std::string("INPUT"));
  CHECK_EQ(std::string(simcore::sideRoleName(SideRole::OUTPUT)), std::string("OUTPUT"));
  CHECK_EQ(std::string(simcore::sideRoleName(SideRole::ENERGY)), std::string("ENERGY"));
  CHECK_EQ(std::string(simcore::sideRoleName(SideRole::FLUID_IN)), std::string("FLUID_IN"));
  CHECK_EQ(std::string(simcore::sideRoleName(SideRole::FLUID_OUT)), std::string("FLUID_OUT"));
  CHECK_EQ(std::string(simcore::sideRoleName(SideRole::ANY)), std::string("ANY"));
  CHECK_EQ(std::string(simcore::sideRoleName(SideRole::NONE)), std::string("NONE"));
}

static void test_sideRoleName_falls_back_for_out_of_range_values() {
  CHECK_EQ(std::string(simcore::sideRoleName(static_cast<SideRole>(7))), std::string("UNKNOWN"));
  CHECK_EQ(std::string(simcore::sideRoleName(static_cast<SideRole>(255))), std::string("UNKNOWN"));
}

static void test_nextSideRole_INPUT_branches_on_available_resources() {
  // hasEnergy wins over hasFluid; otherwise fluid; otherwise plain output.
  CHECK_EQ(simcore::nextSideRole(0, false, false), static_cast<uint8_t>(1));
  CHECK_EQ(simcore::nextSideRole(0, true, false), static_cast<uint8_t>(3));
  CHECK_EQ(simcore::nextSideRole(0, false, true), static_cast<uint8_t>(2));
}

static void test_nextSideRole_OUTPUT_and_ANY_return_to_INPUT() {
  CHECK_EQ(simcore::nextSideRole(1, false, false), static_cast<uint8_t>(0));
  CHECK_EQ(simcore::nextSideRole(1, true, true), static_cast<uint8_t>(0));
  CHECK_EQ(simcore::nextSideRole(5, false, false), static_cast<uint8_t>(0));
  CHECK_EQ(simcore::nextSideRole(5, true, true), static_cast<uint8_t>(0));
}

static void test_nextSideRole_ENERGY_goes_to_ANY() {
  // ENERGY -> ANY regardless of capabilities, so an energy-configured face
  // never steps through fluid roles.
  CHECK_EQ(simcore::nextSideRole(2, false, false), static_cast<uint8_t>(5));
  CHECK_EQ(simcore::nextSideRole(2, true, true), static_cast<uint8_t>(5));
}

static void test_nextSideRole_fluid_roles_cycle_between_themselves() {
  // FLUID_IN -> FLUID_OUT unconditionally...
  CHECK_EQ(simcore::nextSideRole(3, false, false), static_cast<uint8_t>(4));
  CHECK_EQ(simcore::nextSideRole(3, true, true), static_cast<uint8_t>(4));
  // ...and FLUID_OUT -> FLUID_IN only while the machine actually has fluid.
  CHECK_EQ(simcore::nextSideRole(4, true, false), static_cast<uint8_t>(3));
  CHECK_EQ(simcore::nextSideRole(4, true, true), static_cast<uint8_t>(3));
  // Without fluid, FLUID_OUT falls back to INPUT.
  CHECK_EQ(simcore::nextSideRole(4, false, false), static_cast<uint8_t>(0));
  CHECK_EQ(simcore::nextSideRole(4, false, true), static_cast<uint8_t>(0));
}

static void test_nextSideRole_NONE_and_out_of_range_reset_to_ANY() {
  CHECK_EQ(simcore::nextSideRole(6, false, false), static_cast<uint8_t>(5));
  CHECK_EQ(simcore::nextSideRole(6, true, true), static_cast<uint8_t>(5));
  CHECK_EQ(simcore::nextSideRole(7, false, false), static_cast<uint8_t>(5));
  CHECK_EQ(simcore::nextSideRole(200, true, true), static_cast<uint8_t>(5));
}

// OBSERVED DEVIATION: on a machine with BOTH energy and fluid, the wrench
// cycle is INPUT -> ENERGY -> ANY -> INPUT -> ... and FLUID_IN/FLUID_OUT are
// unreachable from the default state, because the hasEnergy branch is checked
// first. Starting from FLUID_IN they are still reachable (case 3), so the
// reachable set depends on the starting face.
static void test_nextSideRole_energy_and_fluid_machine_cannot_reach_fluid_from_INPUT() {
  CHECK_EQ(simcore::nextSideRole(0, true, true), static_cast<uint8_t>(2));  // -> ENERGY
  CHECK_EQ(simcore::nextSideRole(2, true, true), static_cast<uint8_t>(5));  // -> ANY
  CHECK_EQ(simcore::nextSideRole(5, true, true), static_cast<uint8_t>(0));  // -> INPUT
  // Fluid is reachable only from a face already set to FLUID_IN.
  CHECK_EQ(simcore::nextSideRole(3, true, true), static_cast<uint8_t>(4));
}

// OBSERVED DEVIATION: a machine with neither energy nor fluid cycles
// INPUT <-> OUTPUT forever and never reaches ANY or NONE through this
// helper, so such a face can never be blanked by wrenching it.
static void test_nextSideRole_plain_machine_two_cycles_between_input_and_output() {
  uint8_t role = 0;  // INPUT
  for (int step = 0; step < 6; ++step) {
    role = simcore::nextSideRole(role, false, false);
    CHECK(role == 1 || role == 0);
  }
  CHECK_EQ(role, static_cast<uint8_t>(0));
}

// ===========================================================================
// gp-ug4 — MachineComponent.h
// ===========================================================================

static void test_MachineFaceRole_declared_ordinals() {
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::NONE), 0u);
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::INPUT), 1u);
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::OUTPUT), 2u);
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::ENERGY), 3u);
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::FLUID_IN), 4u);
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::FLUID_OUT), 5u);
}

static void test_MachineComponent_defaults_are_all_zero() {
  const MachineComponent machine;
  CHECK_EQ(machine.machine_id, static_cast<uint16_t>(0));
  CHECK_EQ(machine.mb_id, static_cast<uint32_t>(0));
  CHECK_EQ(machine.x, static_cast<uint32_t>(0));
  CHECK_EQ(machine.y, static_cast<uint32_t>(0));
  CHECK_EQ(machine.z, static_cast<uint32_t>(0));
  CHECK_EQ(machine.machine_instance_id, static_cast<uint64_t>(0));
  CHECK_EQ(machine.managed_externally, false);
  for (int face = 0; face < 6; ++face) {
    CHECK_EQ(static_cast<int>(machine.side_config[face]), 0);
  }
}

static void test_MachineComponent_full_constructor_stores_identity_and_position() {
  const MachineComponent machine(static_cast<uint16_t>(1005), 77u, 10u, 64u, 0xFFFFFFFFu,
                                 static_cast<uint64_t>(0xABCDEF));
  CHECK_EQ(machine.machine_id, static_cast<uint16_t>(1005));
  CHECK_EQ(machine.mb_id, static_cast<uint32_t>(77));
  CHECK_EQ(machine.x, static_cast<uint32_t>(10));
  CHECK_EQ(machine.y, static_cast<uint32_t>(64));
  CHECK_EQ(machine.z, static_cast<uint32_t>(0xFFFFFFFF));
  CHECK_EQ(machine.machine_instance_id, static_cast<uint64_t>(0xABCDEF));
}

static void test_MachineComponent_constructor_leaves_optional_fields_defaulted() {
  // The full constructor does not list managed_externally or side_config, so
  // their default member initialisers apply: not externally managed, all
  // faces NONE.
  const MachineComponent machine(1u, 0u, 0u, 0u, 0u, 0u);
  CHECK_EQ(machine.managed_externally, false);
  for (int face = 0; face < 6; ++face) {
    CHECK_EQ(static_cast<int>(machine.side_config[face]), 0);
  }
}

static void test_MachineComponent_face_roles_round_trip() {
  MachineComponent machine(1u, 0u, 0u, 0u, 0u, 0u);
  // GTNH face order: DOWN(0) UP(1) NORTH(2) SOUTH(3) WEST(4) EAST(5)
  machine.setFaceRole(0, static_cast<uint8_t>(MachineFaceRole::INPUT));
  machine.setFaceRole(1, static_cast<uint8_t>(MachineFaceRole::OUTPUT));
  machine.setFaceRole(2, static_cast<uint8_t>(MachineFaceRole::ENERGY));
  machine.setFaceRole(3, static_cast<uint8_t>(MachineFaceRole::FLUID_IN));
  machine.setFaceRole(4, static_cast<uint8_t>(MachineFaceRole::FLUID_OUT));
  machine.setFaceRole(5, static_cast<uint8_t>(MachineFaceRole::NONE));

  CHECK_EQ(static_cast<int>(machine.getFaceRole(0)), 1);
  CHECK_EQ(static_cast<int>(machine.getFaceRole(1)), 2);
  CHECK_EQ(static_cast<int>(machine.getFaceRole(2)), 3);
  CHECK_EQ(static_cast<int>(machine.getFaceRole(3)), 4);
  CHECK_EQ(static_cast<int>(machine.getFaceRole(4)), 5);
  CHECK_EQ(static_cast<int>(machine.getFaceRole(5)), 0);
}

static void test_MachineComponent_face_accessors_bounds_check_to_six() {
  MachineComponent machine(1u, 0u, 0u, 0u, 0u, 0u);
  // Out-of-range reads report NONE (0) and out-of-range writes are dropped.
  for (uint8_t face = 6; face < 12; ++face) {
    CHECK_EQ(static_cast<int>(machine.getFaceRole(face)), 0);
    machine.setFaceRole(face, static_cast<uint8_t>(MachineFaceRole::OUTPUT));
    CHECK_EQ(static_cast<int>(machine.getFaceRole(face)), 0);
  }
  // The in-range faces were untouched by the rejected writes.
  for (int face = 0; face < 6; ++face) {
    CHECK_EQ(static_cast<int>(machine.getFaceRole(static_cast<uint8_t>(face))), 0);
  }
  // uint8_t is unsigned, so 255 is the largest face index tested.
  machine.setFaceRole(255, static_cast<uint8_t>(MachineFaceRole::ENERGY));
  CHECK_EQ(static_cast<int>(machine.getFaceRole(255)), 0);
}

static void test_MachineComponent_side_config_is_independently_addressable() {
  MachineComponent machine(1u, 0u, 0u, 0u, 0u, 0u);
  for (uint8_t face = 0; face < 6; ++face) {
    machine.setFaceRole(face, static_cast<uint8_t>(face + 1));
  }
  for (uint8_t face = 0; face < 6; ++face) {
    CHECK_EQ(static_cast<int>(machine.side_config[face]), static_cast<int>(face) + 1);
  }
}

// OBSERVED DEVIATION: MachineComponent::side_config defaults to all-zero
// (all MachineFaceRole::NONE), while SideConfig::DEFAULT_SIDE_CONFIG is all
// 5 (all SideRole::ANY) and HatchSlot::side_config also defaults to 5. The two
// enums are NOT interchangeable — MachineComponent uses NONE=0..FLUID_OUT=5
// while SideRole uses INPUT=0..NONE=6 — yet both are stored as raw uint8_t in
// side_config[]. A caller that writes a SideRole ordinal into
// MachineComponent::side_config writes a different role entirely.
static void test_MachineComponent_default_side_config_disagrees_with_DEFAULT_SIDE_CONFIG() {
  const MachineComponent machine;
  for (int face = 0; face < 6; ++face) {
    // MachineComponent default: 0 == MachineFaceRole::NONE.
    CHECK_EQ(static_cast<int>(machine.side_config[face]), 0);
    // SideConfig default for the same face index: 5 == SideRole::ANY.
    CHECK_EQ(static_cast<int>(simcore::DEFAULT_SIDE_CONFIG[face]), 5);
    CHECK_NE(static_cast<int>(machine.side_config[face]),
             static_cast<int>(simcore::DEFAULT_SIDE_CONFIG[face]));
  }
  // The two enums disagree on what ordinal 0 even means.
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::NONE), static_cast<uint8_t>(SideRole::INPUT));
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::INPUT), static_cast<uint8_t>(SideRole::OUTPUT));
  CHECK_EQ(static_cast<uint8_t>(MachineFaceRole::FLUID_OUT), static_cast<uint8_t>(SideRole::ANY));
}

static void test_MachineComponent_managed_externally_flag_is_a_plain_bool() {
  MachineComponent machine(1u, 0u, 0u, 0u, 0u, 0u);
  CHECK_EQ(machine.managed_externally, false);
  machine.managed_externally = true;
  CHECK_EQ(machine.managed_externally, true);
}

// ===========================================================================
// gp-i8f — HatchSlot.h
// ===========================================================================

static void test_HatchSlot_defaults_match_the_observed_state() {
  const HatchSlot hatch;
  CHECK_EQ(static_cast<uint8_t>(hatch.type), static_cast<uint8_t>(HatchType::NONE));
  CHECK_EQ(hatch.world_x, static_cast<uint32_t>(0));
  CHECK_EQ(hatch.world_y, static_cast<uint32_t>(0));
  CHECK_EQ(hatch.world_z, static_cast<uint32_t>(0));
  CHECK_EQ(hatch.slot_start, static_cast<uint16_t>(0));
  CHECK_EQ(hatch.slot_end, static_cast<uint16_t>(0));
  CHECK_EQ(hatch.side_config, static_cast<uint8_t>(5));
  CHECK_EQ(hatch.tier, static_cast<uint8_t>(0));
  CHECK_EQ(hatch.present, false);
}

static void test_HatchSlot_kSlotsPerHatch_is_four_for_item_hatches_only() {
  CHECK_EQ(HatchSlot::kSlotsPerHatch(HatchType::ITEM_IN), static_cast<uint16_t>(4));
  CHECK_EQ(HatchSlot::kSlotsPerHatch(HatchType::ITEM_OUT), static_cast<uint16_t>(4));
  // Everything else — fluid, energy, muffler and NONE — exposes no item slots.
  CHECK_EQ(HatchSlot::kSlotsPerHatch(HatchType::FLUID_IN), static_cast<uint16_t>(0));
  CHECK_EQ(HatchSlot::kSlotsPerHatch(HatchType::FLUID_OUT), static_cast<uint16_t>(0));
  CHECK_EQ(HatchSlot::kSlotsPerHatch(HatchType::ENERGY), static_cast<uint16_t>(0));
  CHECK_EQ(HatchSlot::kSlotsPerHatch(HatchType::MUFFLER), static_cast<uint16_t>(0));
  CHECK_EQ(HatchSlot::kSlotsPerHatch(HatchType::NONE), static_cast<uint16_t>(0));
}

static void test_HatchSlot_kSlotsPerHatch_is_usable_in_constant_expressions() {
  static_assert(HatchSlot::kSlotsPerHatch(HatchType::ITEM_IN) == 4,
                "item-in hatches expose four slots");
  static_assert(HatchSlot::kSlotsPerHatch(HatchType::ENERGY) == 0,
                "energy hatches expose no item slots");
  CHECK_EQ(static_cast<int>(HatchSlot::kSlotsPerHatch(HatchType::ITEM_OUT)), 4);
}

static void test_HatchSlot_hasItemSlots_tracks_kSlotsPerHatch() {
  HatchSlot item_in;
  item_in.type = HatchType::ITEM_IN;
  CHECK(item_in.hasItemSlots());

  HatchSlot item_out;
  item_out.type = HatchType::ITEM_OUT;
  CHECK(item_out.hasItemSlots());

  HatchSlot fluid;
  fluid.type = HatchType::FLUID_IN;
  CHECK(!fluid.hasItemSlots());

  const HatchSlot none;
  CHECK(!none.hasItemSlots());
}

static void test_HatchSlot_absent_hatch_still_reports_its_geometry() {
  // present is the authoritative gate; a missing hatch is still fully
  // addressable, so a caller that forgets the check reads a valid-looking
  // world position with present == false.
  HatchSlot hatch;
  hatch.type = HatchType::ITEM_IN;
  hatch.world_x = 100u;
  hatch.world_y = 64u;
  hatch.world_z = 0xFFFFFFFFu;
  hatch.slot_start = 4;
  hatch.slot_end = 7;
  CHECK_EQ(hatch.present, false);
  CHECK(hatch.hasItemSlots());
  CHECK_EQ(hatch.slot_end - hatch.slot_start, static_cast<uint16_t>(3));
}

// OBSERVED: the default side_config is 5, which equals SideRole::ANY and
// MachineFaceRole::FLUID_OUT — see the MachineComponent/SideConfig enum
// mismatch pinned above. It does not equal either enum's NONE.
static void test_HatchSlot_default_side_config_is_five() {
  const HatchSlot hatch;
  CHECK_EQ(static_cast<int>(hatch.side_config), 5);
  CHECK_EQ(static_cast<int>(simcore::DEFAULT_SIDE_CONFIG[0]), static_cast<int>(hatch.side_config));
  CHECK_NE(static_cast<int>(hatch.side_config), static_cast<uint8_t>(MachineFaceRole::NONE));
}

// ===========================================================================
// gp-38d — InventoryContainer.h
// ===========================================================================

static void test_InventorySlot_defaults_are_empty() {
  const InventorySlot slot;
  CHECK_EQ(slot.item_id, static_cast<uint16_t>(0));
  CHECK_EQ(slot.count, static_cast<uint8_t>(0));
  CHECK_EQ(slot.meta, static_cast<uint16_t>(0));
}

static void test_InventorySlot_constructor_stores_all_fields() {
  const InventorySlot slot(static_cast<uint16_t>(1234), static_cast<uint8_t>(64),
                           static_cast<uint16_t>(7));
  CHECK_EQ(slot.item_id, static_cast<uint16_t>(1234));
  CHECK_EQ(slot.count, static_cast<uint8_t>(64));
  CHECK_EQ(slot.meta, static_cast<uint16_t>(7));
}

static void test_InventoryContainer_defaults_are_empty() {
  const InventoryContainer container;
  CHECK_EQ(container.entity_type, static_cast<uint16_t>(0));
  CHECK_EQ(container.slot_count, static_cast<uint16_t>(0));
  CHECK_EQ(container.slots.size(), static_cast<size_t>(0));
}

static void test_InventoryContainer_constructor_grows_slots_to_slot_count() {
  const std::vector<InventorySlot> items{InventorySlot(1, 5, 0)};
  const InventoryContainer container(static_cast<uint16_t>(0), static_cast<uint16_t>(4), items);
  CHECK_EQ(container.entity_type, static_cast<uint16_t>(0));
  CHECK_EQ(container.slot_count, static_cast<uint16_t>(4));
  CHECK_EQ(container.slots.size(), static_cast<size_t>(4));
  // The supplied slot survives; the rest are default-empty.
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(1));
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(5));
  for (size_t i = 1; i < container.slots.size(); ++i) {
    CHECK_EQ(static_cast<int>(container.slots[i].count), 0);
  }
}

static void test_InventoryContainer_constructor_never_shrinks_slots() {
  // The resize guard is one-directional, so slot_count and slots.size() can
  // disagree; slot_count is not authoritative.
  const std::vector<InventorySlot> items{InventorySlot(1, 1, 0), InventorySlot(2, 2, 0),
                                         InventorySlot(3, 3, 0)};
  const InventoryContainer container(0, static_cast<uint16_t>(1), items);
  CHECK_EQ(container.slot_count, static_cast<uint16_t>(1));
  CHECK_EQ(container.slots.size(), static_cast<size_t>(3));
  // Indices past slot_count are still readable through getSlot.
  CHECK_EQ(container.getSlot(2).item_id, static_cast<uint16_t>(3));
}

static void test_InventoryContainer_getSlot_out_of_range_returns_empty_slot() {
  const std::vector<InventorySlot> items{InventorySlot(9, 9, 9)};
  const InventoryContainer container(0, 1, items);
  CHECK_EQ(container.getSlot(0).item_id, static_cast<uint16_t>(9));
  CHECK_EQ(container.getSlot(1).count, static_cast<uint8_t>(0));
  CHECK_EQ(container.getSlot(65535).meta, static_cast<uint16_t>(0));
}

static void test_InventoryContainer_setSlot_grows_the_slot_vector() {
  InventoryContainer container(0, 1, std::vector<InventorySlot>{});
  CHECK_EQ(container.slots.size(), static_cast<size_t>(1));
  container.setSlot(3, InventorySlot(77, 12, 34));
  CHECK_EQ(container.slots.size(), static_cast<size_t>(4));
  CHECK_EQ(container.slots[3].item_id, static_cast<uint16_t>(77));
  CHECK_EQ(container.slots[3].count, static_cast<uint8_t>(12));
  CHECK_EQ(container.slots[3].meta, static_cast<uint16_t>(34));
  // The growth introduced empty slots, not garbage.
  for (size_t i = 0; i < 3; ++i) {
    CHECK_EQ(static_cast<int>(container.slots[i].count), 0);
  }
  // setSlot overwrites in place without changing the size.
  container.setSlot(3, InventorySlot(88, 1, 0));
  CHECK_EQ(container.slots.size(), static_cast<size_t>(4));
  CHECK_EQ(container.slots[3].item_id, static_cast<uint16_t>(88));
}

static void test_InventoryContainer_addItem_fills_the_first_empty_slot() {
  InventoryContainer container(0, 3, std::vector<InventorySlot>{});
  const int32_t leftover = container.addItem(static_cast<uint16_t>(100),
                                             static_cast<uint8_t>(10),
                                             static_cast<uint16_t>(4));
  CHECK_EQ(leftover, static_cast<int32_t>(0));
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(100));
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(10));
  CHECK_EQ(container.slots[0].meta, static_cast<uint16_t>(4));
  CHECK_EQ(container.slots[1].item_id, static_cast<uint16_t>(0));
}

static void test_InventoryContainer_addItem_stacks_into_a_matching_slot() {
  const std::vector<InventorySlot> items{InventorySlot(50, 10, 3)};
  InventoryContainer container(0, 2, items);
  container.addItem(50, 10, 3);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(20));
  CHECK_EQ(container.slots[0].meta, static_cast<uint16_t>(3));
  // The second slot was not touched.
  CHECK_EQ(static_cast<int>(container.slots[1].count), 0);
}

// OBSERVED DEVIATION: addItem's "amount left over" return value is computed as
// `uint8_t added = (slot.count + count) - 64`, so a stack that does NOT
// overflow still returns a wrapped byte. Adding 10 to a 10-stack reports 207
// items "left over" even though everything was stored. Any caller that loops
// on a non-zero return will spin or mis-account. The stored state is correct;
// only the return value is wrong.
static void test_InventoryContainer_addItem_reports_wrapped_leftover_when_not_full() {
  const std::vector<InventorySlot> items{InventorySlot(50, 10, 0)};
  InventoryContainer container(0, 2, items);
  const int32_t leftover = container.addItem(50, 10, 0);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(20));
  CHECK_EQ(leftover, static_cast<int32_t>(212));  // (10 + 10) - 64 == -44 as uint8_t
}

static void test_InventoryContainer_addItem_overflow_clamps_to_64() {
  const std::vector<InventorySlot> items{InventorySlot(50, 60, 3)};
  InventoryContainer container(0, 2, items);
  const int32_t leftover = container.addItem(50, 10, 9);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(64));
  CHECK_EQ(leftover, static_cast<int32_t>(6));  // (60 + 10) - 64
  // OBSERVED: clamping to 64 also overwrites the slot's meta with the
  // incoming meta, destroying the original 3.
  CHECK_EQ(container.slots[0].meta, static_cast<uint16_t>(9));
}

static void test_InventoryContainer_addItem_to_a_full_stack_spills_nowhere() {
  const std::vector<InventorySlot> items{InventorySlot(50, 64, 0), InventorySlot(0, 0, 0)};
  InventoryContainer container(0, 2, items);
  const int32_t leftover = container.addItem(50, 5, 0);
  // The matching-stack loop requires count < 64, so a full stack is skipped
  // and the empty-slot loop takes it instead.
  CHECK_EQ(leftover, static_cast<int32_t>(0));
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(64));
  CHECK_EQ(container.slots[1].item_id, static_cast<uint16_t>(50));
  CHECK_EQ(container.slots[1].count, static_cast<uint8_t>(5));
}

static void test_InventoryContainer_addItem_on_a_full_container_returns_everything() {
  const std::vector<InventorySlot> items{InventorySlot(1, 64, 0), InventorySlot(2, 64, 0)};
  InventoryContainer container(0, 2, items);
  const int32_t leftover = container.addItem(static_cast<uint16_t>(99),
                                             static_cast<uint8_t>(7),
                                             static_cast<uint16_t>(0));
  CHECK_EQ(leftover, static_cast<int32_t>(7));
  for (size_t i = 0; i < container.slots.size(); ++i) {
    CHECK_NE(static_cast<int>(container.slots[i].item_id), 99);
  }
}

// OBSERVED: the empty-slot branch clamps the stored count to 64 but still
// returns 0 "left over", so a 100-unit add into an empty slot silently
// discards 36 units while claiming success.
static void test_InventoryContainer_addItem_oversized_into_empty_slot_discards_excess() {
  InventoryContainer container(0, 2, std::vector<InventorySlot>{});
  const int32_t leftover = container.addItem(60, 100, 0);
  CHECK_EQ(leftover, static_cast<int32_t>(0));
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(60));
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(64));
}

static void test_InventoryContainer_addItem_ignores_meta_when_choosing_a_stack() {
  // Stacking is matched on item_id alone; the same id with a different meta
  // still merges into the existing stack.
  const std::vector<InventorySlot> items{InventorySlot(70, 10, 1)};
  InventoryContainer container(0, 2, items);
  container.addItem(70, 5, 2);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(15));
  // No overflow, so the slot's original meta is preserved.
  CHECK_EQ(container.slots[0].meta, static_cast<uint16_t>(1));
}

static void test_InventoryContainer_addItem_with_zero_count_also_reports_wrapped_leftover() {
  const std::vector<InventorySlot> items{InventorySlot(80, 10, 0)};
  InventoryContainer container(0, 2, items);
  const int32_t leftover = container.addItem(80, 0, 0);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(10));
  CHECK_EQ(leftover, static_cast<int32_t>(202));  // (10 + 0) - 64 == -54 as uint8_t
}

static void test_InventoryContainer_addItem_of_item_id_zero_never_fills_a_slot() {
  // OBSERVED: item_id 0 means "empty", so the stacking loop matches the empty
  // slot itself and the empty-slot loop can never fire. Adding the item whose
  // id is 0 writes into slot 0's count without ever being placed as a real
  // item, and the wrapped leftover is returned.
  InventoryContainer container(0, 2, std::vector<InventorySlot>{});
  const int32_t leftover = container.addItem(0, 5, 0);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(5));
  CHECK_EQ(leftover, static_cast<int32_t>(197));  // (0 + 5) - 64 == -59 as uint8_t
}

static void test_InventoryContainer_addItem_fills_successive_empty_slots() {
  InventoryContainer container(0, 3, std::vector<InventorySlot>{});
  container.addItem(1, 10, 0);
  container.addItem(2, 10, 0);
  container.addItem(3, 10, 0);
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(1));
  CHECK_EQ(container.slots[1].item_id, static_cast<uint16_t>(2));
  CHECK_EQ(container.slots[2].item_id, static_cast<uint16_t>(3));
  // A fourth distinct item has nowhere to go.
  CHECK_EQ(container.addItem(4, 10, 0), static_cast<int32_t>(10));
}

static void test_InventoryContainer_removeItem_rejects_out_of_range_index() {
  const std::vector<InventorySlot> items{InventorySlot(1, 10, 0)};
  InventoryContainer container(0, 1, items);
  CHECK_EQ(container.removeItem(1, 1), false);
  CHECK_EQ(container.removeItem(65535, 1), false);
  CHECK_EQ(container.slots.size(), static_cast<size_t>(1));
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(10));
}

static void test_InventoryContainer_removeItem_rejects_more_than_present() {
  const std::vector<InventorySlot> items{InventorySlot(1, 10, 0)};
  InventoryContainer container(0, 1, items);
  CHECK_EQ(container.removeItem(0, 11), false);
  CHECK_EQ(container.removeItem(0, 255), false);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(10));
}

static void test_InventoryContainer_removeItem_partial_removal_keeps_the_slot() {
  const std::vector<InventorySlot> items{InventorySlot(1, 10, 0)};
  InventoryContainer container(0, 1, items);
  CHECK_EQ(container.removeItem(0, 4), true);
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(6));
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(1));
  CHECK_EQ(container.slots.size(), static_cast<size_t>(1));
}

// OBSERVED DEVIATION: emptying a slot ERASES it from the vector instead of
// zeroing it, so every later index shifts down by one. Slot indices are not
// stable across a removal, and slot_count is not updated either.
static void test_InventoryContainer_removeItem_erases_and_shifts_later_slots() {
  const std::vector<InventorySlot> items{InventorySlot(1, 5, 0), InventorySlot(2, 7, 0),
                                         InventorySlot(3, 9, 0)};
  InventoryContainer container(0, 3, items);
  CHECK_EQ(container.removeItem(0, 5), true);
  CHECK_EQ(container.slots.size(), static_cast<size_t>(2));
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(2));
  CHECK_EQ(container.slots[1].item_id, static_cast<uint16_t>(3));
  // slot_count still claims three slots for a two-slot vector.
  CHECK_EQ(container.slot_count, static_cast<uint16_t>(3));
  // getSlot past the (now shorter) vector is still the empty-slot fallback.
  CHECK_EQ(container.getSlot(2).item_id, static_cast<uint16_t>(0));
}

static void test_InventoryContainer_removeItem_zero_count_is_a_no_op() {
  const std::vector<InventorySlot> items{InventorySlot(1, 5, 0), InventorySlot(2, 7, 0)};
  InventoryContainer container(0, 2, items);
  // count 0 is not "more than present", so the removal is accepted, but
  // `slot.count -= 0` leaves 5 and the erase branch (count == 0) does not fire.
  CHECK_EQ(container.removeItem(0, 0), true);
  CHECK_EQ(container.slots.size(), static_cast<size_t>(2));
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(1));
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(5));
}

static void test_InventoryContainer_removeItem_of_empty_slot_erases_it() {
  const std::vector<InventorySlot> items{InventorySlot(0, 0, 0), InventorySlot(2, 7, 0)};
  InventoryContainer container(0, 2, items);
  CHECK_EQ(container.removeItem(0, 0), true);
  CHECK_EQ(container.slots.size(), static_cast<size_t>(1));
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(2));
}

static void test_InventoryContainer_full_insert_and_remove_cycle() {
  InventoryContainer container(0, 2, std::vector<InventorySlot>{});
  container.addItem(1, 64, 0);
  container.addItem(2, 64, 0);
  CHECK_EQ(container.addItem(3, 1, 0), static_cast<int32_t>(1));  // both slots full

  CHECK_EQ(container.removeItem(0, 64), true);
  // Emptying slot 0 ERASED it, so the vector is now one element long and the
  // surviving item 2 slid down into index 0. There is no free slot: the
  // container is now permanently one slot short, because the erase left a hole
  // rather than a cleared slot.
  CHECK_EQ(container.slots.size(), static_cast<size_t>(1));
  CHECK_EQ(container.slots[0].item_id, static_cast<uint16_t>(2));
  CHECK_EQ(container.slots[0].count, static_cast<uint8_t>(64));
  // slot_count still claims 2, but the vector holds 1, and a new item has
  // nowhere to go.
  CHECK_EQ(container.slot_count, static_cast<uint16_t>(2));
  CHECK_EQ(container.addItem(3, 1, 0), static_cast<int32_t>(1));
  CHECK_EQ(container.slots.size(), static_cast<size_t>(1));
}

static void test_InventoryContainer_entity_type_is_opaque_metadata() {
  // 0=chest, 1=furnace, 2=electrolyser is a comment convention only; the
  // struct neither validates nor branches on it.
  const InventoryContainer chest(0, 1, std::vector<InventorySlot>{});
  const InventoryContainer electrolyser(2, 1, std::vector<InventorySlot>{});
  const InventoryContainer unknown(999, 1, std::vector<InventorySlot>{});
  CHECK_EQ(chest.entity_type, static_cast<uint16_t>(0));
  CHECK_EQ(electrolyser.entity_type, static_cast<uint16_t>(2));
  CHECK_EQ(unknown.entity_type, static_cast<uint16_t>(999));
  CHECK_EQ(chest.slots.size(), unknown.slots.size());
}

static void test_InventoryContainer_independent_copies_do_not_alias() {
  InventoryContainer original(0, 2, std::vector<InventorySlot>{});
  original.addItem(5, 10, 0);
  InventoryContainer copy = original;
  copy.setSlot(0, InventorySlot(6, 1, 0));
  CHECK_EQ(original.slots[0].item_id, static_cast<uint16_t>(5));
  CHECK_EQ(copy.slots[0].item_id, static_cast<uint16_t>(6));
}

// ===========================================================================
// gp-kp4 — MultiblockController.h
// ===========================================================================

static void test_MultiblockController_defaults_are_empty() {
  const MultiblockController controller;
  CHECK_EQ(controller.id, static_cast<uint64_t>(0));
  CHECK_EQ(controller.x, static_cast<uint32_t>(0));
  CHECK_EQ(controller.y, static_cast<uint32_t>(0));
  CHECK_EQ(controller.z, static_cast<uint32_t>(0));
  CHECK_EQ(controller.pattern_id, static_cast<uint32_t>(0));
  CHECK_EQ(controller.blocks.size(), static_cast<size_t>(0));
  CHECK_EQ(controller.hatches.size(), static_cast<size_t>(0));
}

static void test_MultiblockController_constructor_stores_anchor_and_pattern() {
  const std::vector<uint32_t> blocks{1u, 2u, 3u, 4u, 5u};
  const MultiblockController controller(42u, 100u, 64u, 0xFFFFFFFFu, 7u, blocks);
  CHECK_EQ(controller.id, static_cast<uint64_t>(42));
  CHECK_EQ(controller.x, static_cast<uint32_t>(100));
  CHECK_EQ(controller.y, static_cast<uint32_t>(64));
  CHECK_EQ(controller.z, static_cast<uint32_t>(0xFFFFFFFF));
  CHECK_EQ(controller.pattern_id, static_cast<uint32_t>(7));
  CHECK_EQ(controller.blocks.size(), static_cast<size_t>(5));
  CHECK_EQ(controller.blocks[4], static_cast<uint32_t>(5));
  // hatches is never touched by the constructor.
  CHECK_EQ(controller.hatches.size(), static_cast<size_t>(0));
}

static void test_MultiblockController_constructor_takes_a_copy_of_blocks() {
  std::vector<uint32_t> source{1u, 2u};
  const MultiblockController controller(1u, 0u, 0u, 0u, 0u, source);
  source.push_back(3u);
  // The controller kept its own copy, so later edits to the source vector do
  // not leak into the serialized structure.
  CHECK_EQ(controller.blocks.size(), static_cast<size_t>(2));
  CHECK_EQ(source.size(), static_cast<size_t>(3));
}

static void test_MultiblockController_is_anchored_at_its_own_position() {
  // The anchor is the controller block; blocks[] holds the packed positions
  // of every other member block (ChunkStore stores mb_id in the meta layer).
  const std::vector<uint32_t> blocks{1u, 2u};
  const MultiblockController controller(9u, 5u, 6u, 7u, 1u, blocks);
  CHECK_NE(controller.x, controller.blocks[0]);
  CHECK_EQ(controller.x, static_cast<uint32_t>(5));
  CHECK_EQ(controller.blocks[0], static_cast<uint32_t>(1));
}

static void test_MultiblockController_hatches_are_appended_after_construction() {
  MultiblockController controller(1u, 0u, 0u, 0u, 0u, std::vector<uint32_t>{});
  HatchSlot input;
  input.type = HatchType::ITEM_IN;
  input.world_x = 10u;
  input.slot_start = 0;
  input.slot_end = 3;
  input.present = true;

  HatchSlot energy;
  energy.type = HatchType::ENERGY;
  energy.present = true;

  controller.hatches.push_back(input);
  controller.hatches.push_back(energy);

  CHECK_EQ(controller.hatches.size(), static_cast<size_t>(2));
  CHECK_EQ(static_cast<uint8_t>(controller.hatches[0].type),
           static_cast<uint8_t>(HatchType::ITEM_IN));
  CHECK(controller.hatches[0].hasItemSlots());
  CHECK_EQ(controller.hatches[0].slot_end - controller.hatches[0].slot_start,
           static_cast<uint16_t>(3));
  CHECK_EQ(static_cast<uint8_t>(controller.hatches[1].type),
           static_cast<uint8_t>(HatchType::ENERGY));
  CHECK(!controller.hatches[1].hasItemSlots());
  CHECK_EQ(controller.blocks.size(), static_cast<size_t>(0));
}

static void test_MultiblockController_zero_id_is_distinct_from_a_real_id() {
  const MultiblockController none(0u, 0u, 0u, 0u, 0u, std::vector<uint32_t>{});
  const MultiblockController real(1u, 0u, 0u, 0u, 0u, std::vector<uint32_t>{});
  CHECK_NE(none.id, real.id);
  // Both carry the same anchor; only the id distinguishes them.
  CHECK_EQ(none.x, real.x);
}

// ===========================================================================
// gp-e8p — HeatIntakeComponent.h
// ===========================================================================

static void test_HeatIntake_defaults_are_empty_HEAT_buffer() {
  const HeatIntakeComponent heat;
  CHECK_EQ(static_cast<uint8_t>(heat.input_type), static_cast<uint8_t>(EnergyType::HEAT));
  CHECK_EQ(heat.heat_stored, static_cast<int32_t>(0));
  CHECK_EQ(heat.heat_capacity, static_cast<int32_t>(1000));
}

static void test_HeatIntake_ratio_of_an_empty_buffer_is_zero() {
  const HeatIntakeComponent heat;
  CHECK_NEAR(heat.ratio(), 0.0f, 1e-6f);
}

static void test_HeatIntake_ratio_tracks_stored_over_capacity() {
  HeatIntakeComponent heat;
  heat.heat_stored = 250;
  CHECK_NEAR(heat.ratio(), 0.25f, 1e-6f);
  heat.heat_stored = 500;
  CHECK_NEAR(heat.ratio(), 0.5f, 1e-6f);
  heat.heat_stored = 1000;
  CHECK_NEAR(heat.ratio(), 1.0f, 1e-6f);
}

static void test_HeatIntake_ratio_honours_a_custom_capacity() {
  HeatIntakeComponent heat;
  heat.heat_capacity = 4000;
  heat.heat_stored = 1000;
  CHECK_NEAR(heat.ratio(), 0.25f, 1e-6f);
  heat.heat_capacity = 250;
  heat.heat_stored = 100;
  CHECK_NEAR(heat.ratio(), 0.4f, 1e-6f);
}

static void test_HeatIntake_ratio_is_zero_for_zero_capacity() {
  HeatIntakeComponent heat;
  heat.heat_capacity = 0;
  heat.heat_stored = 0;
  CHECK_NEAR(heat.ratio(), 0.0f, 1e-6f);
  // Even with heat stored, a zero capacity short-circuits to 0 instead of
  // dividing by zero.
  heat.heat_stored = 500;
  CHECK_NEAR(heat.ratio(), 0.0f, 1e-6f);
}

static void test_HeatIntake_ratio_is_zero_for_negative_capacity() {
  HeatIntakeComponent heat;
  heat.heat_capacity = -1000;
  heat.heat_stored = 500;
  CHECK_NEAR(heat.ratio(), 0.0f, 1e-6f);
}

// OBSERVED: ratio() is not clamped, so an over-full buffer reports > 1.0 and
// a negative stored value reports < 0. Callers must clamp if they need a
// 0..1 gauge.
static void test_HeatIntake_ratio_is_unclamped_outside_zero_to_one() {
  HeatIntakeComponent heat;
  heat.heat_stored = 1500;
  CHECK_NEAR(heat.ratio(), 1.5f, 1e-6f);
  heat.heat_stored = -250;
  CHECK_NEAR(heat.ratio(), -0.25f, 1e-6f);
}

static void test_HeatIntake_input_type_is_not_energy_by_default() {
  HeatIntakeComponent heat;
  CHECK_NE(static_cast<int>(heat.input_type), static_cast<int>(EnergyType::ELECTRICITY));
  CHECK_EQ(static_cast<int>(heat.input_type), static_cast<int>(EnergyType::HEAT));
  // The type is plain state and can be retargeted.
  heat.input_type = EnergyType::STEAM;
  CHECK_EQ(static_cast<int>(heat.input_type), static_cast<int>(EnergyType::STEAM));
}

// ===========================================================================
// gp-9pu — SteamOutputComponent.h
// ===========================================================================

static void test_SteamOutput_defaults_are_empty_STEAM_buffer() {
  const SteamOutputComponent steam;
  CHECK_EQ(static_cast<uint8_t>(steam.input_type), static_cast<uint8_t>(EnergyType::STEAM));
  CHECK_NEAR(steam.steam_stored, 0.0, 1e-9);
  CHECK_NEAR(steam.steam_capacity, 1000.0, 1e-9);
}

static void test_SteamOutput_ratio_of_an_empty_buffer_is_zero() {
  const SteamOutputComponent steam;
  CHECK_NEAR(steam.ratio(), 0.0, 1e-9);
}

static void test_SteamOutput_ratio_tracks_stored_over_capacity() {
  SteamOutputComponent steam;
  steam.steam_stored = 250.0;
  CHECK_NEAR(steam.ratio(), 0.25, 1e-9);
  steam.steam_stored = 500.0;
  CHECK_NEAR(steam.ratio(), 0.5, 1e-9);
  steam.steam_stored = 1000.0;
  CHECK_NEAR(steam.ratio(), 1.0, 1e-9);
}

static void test_SteamOutput_ratio_honours_a_custom_capacity() {
  SteamOutputComponent steam;
  steam.steam_capacity = 4000.0;
  steam.steam_stored = 1000.0;
  CHECK_NEAR(steam.ratio(), 0.25, 1e-9);
}

static void test_SteamOutput_ratio_is_zero_for_zero_and_negative_capacity() {
  SteamOutputComponent steam;
  steam.steam_capacity = 0.0;
  steam.steam_stored = 500.0;
  CHECK_NEAR(steam.ratio(), 0.0, 1e-9);

  steam.steam_capacity = -1000.0;
  steam.steam_stored = 500.0;
  CHECK_NEAR(steam.ratio(), 0.0, 1e-9);
}

// Same unclamped ratio() semantics as HeatIntakeComponent, in double.
static void test_SteamOutput_ratio_is_unclamped_outside_zero_to_one() {
  SteamOutputComponent steam;
  steam.steam_stored = 1500.0;
  CHECK_NEAR(steam.ratio(), 1.5, 1e-9);
  steam.steam_stored = -250.0;
  CHECK_NEAR(steam.ratio(), -0.25, 1e-9);
}

// OBSERVED: unlike HeatIntakeComponent (int32_t), steam is stored in double,
// so a boiler's fractional mB/t production needs no scaling trick — but there
// is also no integer step to latch onto.
static void test_SteamOutput_stores_fractional_steam_exactly() {
  SteamOutputComponent steam;
  steam.steam_stored = 0.1;
  steam.steam_stored += 0.2;
  CHECK_NEAR(steam.steam_stored, 0.3, 1e-12);
  CHECK_NEAR(steam.ratio(), 0.0003, 1e-9);
}

static void test_SteamOutput_matches_heat_buffer_shape_with_wider_types() {
  HeatIntakeComponent heat;
  SteamOutputComponent steam;
  CHECK_EQ(sizeof(steam.steam_stored), sizeof(double));
  CHECK_EQ(sizeof(heat.heat_stored), sizeof(int32_t));
  // Both default to their own 1000-unit capacity and a full-buffer ratio of 1.
  steam.steam_stored = 1000.0;
  heat.heat_stored = 1000;
  CHECK_NEAR(steam.ratio(), 1.0, 1e-9);
  CHECK_NEAR(heat.ratio(), 1.0f, 1e-6f);
  CHECK_NE(static_cast<int>(steam.input_type), static_cast<int>(heat.input_type));
}

// ===========================================================================
// gp-9p6 — RecipeProgress.h : RequirementReservation
// ===========================================================================

static void test_RequirementReservation_defaults_are_empty() {
  const RequirementReservation reservation;
  CHECK_EQ(reservation.requirement_id, static_cast<uint32_t>(0));
  CHECK_EQ(static_cast<int>(reservation.kind), static_cast<int>(ResourceKind::FLUID));
  CHECK_EQ(reservation.resource_id, static_cast<uint32_t>(0));
  CHECK_EQ(reservation.required_amount, static_cast<int32_t>(0));
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(0));
  CHECK_EQ(reservation.request_id, static_cast<uint64_t>(0));
  CHECK_EQ(reservation.epoch, static_cast<uint64_t>(0));
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(0));
  CHECK(!reservation.fullyAccepted());
  CHECK(!reservation.partial());
}

static void test_ResourceReservation_is_an_alias_of_RequirementReservation() {
  CHECK((std::is_same<ResourceReservation, RequirementReservation>::value));
}

static void test_RequirementReservation_remainingAmount_tracks_the_gap() {
  RequirementReservation reservation;
  reservation.required_amount = 1000;
  reservation.accepted_amount = 250;
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(750));
  reservation.accepted_amount = 1000;
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(0));
}

static void test_RequirementReservation_remainingAmount_clamps_at_zero() {
  RequirementReservation reservation;
  reservation.required_amount = 100;
  // An over-accepting response must not produce a negative remainder.
  reservation.accepted_amount = 150;
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(0));
  reservation.accepted_amount = -50;
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(150));
}

static void test_RequirementReservation_fullyAccepted_requires_a_positive_requirement() {
  RequirementReservation reservation;
  reservation.required_amount = 0;
  reservation.accepted_amount = 0;
  // required_amount > 0 is part of the predicate, so a zero-amount
  // requirement is never "fully accepted" — the craft can never complete.
  CHECK(!reservation.fullyAccepted());

  reservation.required_amount = 10;
  reservation.accepted_amount = 10;
  CHECK(reservation.fullyAccepted());
  reservation.accepted_amount = 9;
  CHECK(!reservation.fullyAccepted());
}

static void test_RequirementReservation_partial_requires_some_progress() {
  RequirementReservation reservation;
  reservation.required_amount = 100;
  reservation.accepted_amount = 0;
  CHECK(!reservation.partial());

  reservation.accepted_amount = 1;
  CHECK(reservation.partial());

  reservation.accepted_amount = 99;
  CHECK(reservation.partial());

  reservation.accepted_amount = 100;
  CHECK(!reservation.partial());
}

static void test_RequirementReservation_recordAccepted_accumulates_responses() {
  RequirementReservation reservation;
  reservation.required_amount = 1000;
  reservation.recordAccepted(100);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(100));
  reservation.recordAccepted(250);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(350));
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(650));
  CHECK(reservation.partial());
}

static void test_RequirementReservation_recordAccepted_clamps_to_remaining() {
  RequirementReservation reservation;
  reservation.required_amount = 1000;
  reservation.recordAccepted(400);
  // A malformed/duplicate response for 5000 clamps to the 600 still owed.
  reservation.recordAccepted(5000);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(1000));
  CHECK(reservation.fullyAccepted());
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(0));
  // Further responses are absorbed without over-accepting.
  reservation.recordAccepted(1000);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(1000));
}

static void test_RequirementReservation_recordAccepted_ignores_non_positive_responses() {
  RequirementReservation reservation;
  reservation.required_amount = 1000;
  reservation.recordAccepted(0);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(0));
  reservation.recordAccepted(-250);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(0));
  CHECK(!reservation.fullyAccepted());
  CHECK(!reservation.partial());
}

static void test_RequirementReservation_recordAccepted_ignores_zero_requirements() {
  RequirementReservation reservation;
  reservation.required_amount = 0;
  reservation.recordAccepted(500);
  // A requirement of 0 is never fulfilled, so accepting into it is refused.
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(0));
  CHECK(!reservation.fullyAccepted());
}

static void test_RequirementReservation_recordAccepted_ignores_negative_requirements() {
  RequirementReservation reservation;
  reservation.required_amount = -100;
  reservation.recordAccepted(50);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(0));
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(0));
  CHECK(!reservation.fullyAccepted());
}

static void test_RequirementReservation_clear_resets_every_field() {
  RequirementReservation reservation;
  reservation.requirement_id = 5u;
  reservation.kind = ResourceKind::EU;
  reservation.resource_id = 77u;
  reservation.required_amount = 1000;
  reservation.recordAccepted(400);
  reservation.request_id = 0xABCDEFull;
  reservation.epoch = 3u;

  reservation.clear();

  CHECK_EQ(reservation.requirement_id, static_cast<uint32_t>(0));
  CHECK_EQ(static_cast<int>(reservation.kind), static_cast<int>(ResourceKind::FLUID));
  CHECK_EQ(reservation.resource_id, static_cast<uint32_t>(0));
  CHECK_EQ(reservation.required_amount, static_cast<int32_t>(0));
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(0));
  CHECK_EQ(reservation.request_id, static_cast<uint64_t>(0));
  CHECK_EQ(reservation.epoch, static_cast<uint64_t>(0));
  CHECK_EQ(reservation.remainingAmount(), static_cast<int32_t>(0));
  CHECK(!reservation.fullyAccepted());
  CHECK(!reservation.partial());
}

static void test_RequirementReservation_reuse_after_clear_starts_from_zero() {
  RequirementReservation reservation;
  reservation.required_amount = 1000;
  reservation.recordAccepted(1000);
  CHECK(reservation.fullyAccepted());
  reservation.clear();

  reservation.required_amount = 10;
  reservation.recordAccepted(4);
  CHECK_EQ(reservation.accepted_amount, static_cast<int32_t>(4));
  CHECK(!reservation.fullyAccepted());
}

// ===========================================================================
// gp-9p6 — RecipeProgress.h : PendingCraft
// ===========================================================================

static void test_PendingCraft_defaults_are_idle() {
  const PendingCraft craft;
  CHECK_EQ(craft.recipe_id, std::string());
  CHECK_EQ(craft.owner_id, static_cast<uint64_t>(0));
  CHECK_EQ(craft.entity_id, static_cast<uint64_t>(0));
  CHECK_EQ(craft.request_id, static_cast<uint64_t>(0));
  CHECK_EQ(craft.epoch, static_cast<uint64_t>(0));
  CHECK_EQ(craft.retry_count, static_cast<uint32_t>(0));
  CHECK_EQ(craft.expiry_tick, static_cast<uint64_t>(0));
  CHECK_EQ(craft.next_retry_tick, static_cast<uint64_t>(0));
  CHECK_EQ(craft.requirements.size(), static_cast<size_t>(0));
}

static void test_PendingCraft_reservations_accessors_agree() {
  PendingCraft craft;
  craft.requirements.push_back(RequirementReservation());
  craft.requirements[0].requirement_id = 4u;
  CHECK_EQ(craft.reservations().size(), static_cast<size_t>(1));
  CHECK_EQ(craft.reservations()[0].requirement_id, static_cast<uint32_t>(4));

  const PendingCraft& const_craft = craft;
  CHECK_EQ(const_craft.reservations().size(), static_cast<size_t>(1));

  // The mutable accessor really is a view onto requirements.
  craft.reservations()[0].required_amount = 50;
  CHECK_EQ(craft.requirements[0].required_amount, static_cast<int32_t>(50));
}

static void test_PendingCraft_findReservationByRequestId_matches_exactly_one() {
  PendingCraft craft;
  RequirementReservation first;
  first.requirement_id = 1u;
  first.request_id = 0x1111u;
  RequirementReservation second;
  second.requirement_id = 2u;
  second.request_id = 0x2222u;
  craft.requirements.push_back(first);
  craft.requirements.push_back(second);

  RequirementReservation* found = craft.findReservationByRequestId(0x2222u);
  CHECK(found != nullptr);
  if (found != nullptr) {
    CHECK_EQ(found->requirement_id, static_cast<uint32_t>(2));
  }
  CHECK(craft.findReservationByRequestId(0x3333u) == nullptr);
}

static void test_PendingCraft_findReservationByRequestId_ignores_zero_request_ids() {
  PendingCraft craft;
  RequirementReservation unset;
  unset.requirement_id = 1u;
  unset.request_id = 0;
  craft.requirements.push_back(unset);

  // A request id of 0 means "not yet issued", so it must never match — that
  // is what stops a stray response landing on an unissued reservation.
  CHECK(craft.findReservationByRequestId(0) == nullptr);
}

static void test_PendingCraft_findReservationByRequestId_on_empty_craft() {
  PendingCraft craft;
  CHECK(craft.findReservationByRequestId(1234u) == nullptr);
}

static void test_PendingCraft_fullyAccepted_requires_at_least_one_requirement() {
  PendingCraft craft;
  // An empty requirement set is never complete: a craft must not consume
  // input items before it has actually reserved anything.
  CHECK(!craft.fullyAccepted());
  CHECK(!craft.isFullyAccepted());
  CHECK(!craft.partial());
  CHECK(!craft.isPartial());
}

static void test_PendingCraft_fullyAccepted_requires_every_requirement() {
  PendingCraft craft;
  RequirementReservation fluid;
  fluid.required_amount = 1000;
  RequirementReservation energy;
  energy.required_amount = 500;

  craft.requirements.push_back(fluid);
  craft.requirements.push_back(energy);
  CHECK(!craft.fullyAccepted());

  craft.requirements[0].recordAccepted(1000);
  CHECK(!craft.fullyAccepted());
  craft.requirements[1].recordAccepted(500);
  CHECK(craft.fullyAccepted());
  CHECK(craft.isFullyAccepted());
}

static void test_PendingCraft_totals_sum_across_requirements() {
  PendingCraft craft;
  RequirementReservation a;
  a.required_amount = 1000;
  a.accepted_amount = 400;
  RequirementReservation b;
  b.required_amount = 200;
  b.accepted_amount = 200;
  RequirementReservation c;
  c.required_amount = 300;
  c.accepted_amount = 0;
  craft.requirements.push_back(a);
  craft.requirements.push_back(b);
  craft.requirements.push_back(c);

  CHECK_EQ(craft.requiredAmount(), static_cast<int32_t>(1500));
  CHECK_EQ(craft.acceptedAmount(), static_cast<int32_t>(600));
  CHECK_EQ(craft.remainingAmount(), static_cast<int32_t>(900));
  CHECK(craft.partial());
  CHECK(!craft.fullyAccepted());
}

static void test_PendingCraft_totals_ignore_negative_requirements() {
  PendingCraft craft;
  RequirementReservation bad;
  bad.required_amount = -500;
  bad.accepted_amount = 100;
  RequirementReservation good;
  good.required_amount = 100;
  good.accepted_amount = 40;
  craft.requirements.push_back(bad);
  craft.requirements.push_back(good);

  // Negative requirements contribute nothing to either total, and the stray
  // 100 acceptance is clamped away.
  CHECK_EQ(craft.requiredAmount(), static_cast<int32_t>(100));
  CHECK_EQ(craft.acceptedAmount(), static_cast<int32_t>(40));
  CHECK_EQ(craft.remainingAmount(), static_cast<int32_t>(60));
}

static void test_PendingCraft_acceptedAmount_clamps_over_acceptance() {
  PendingCraft craft;
  RequirementReservation over;
  over.required_amount = 100;
  over.accepted_amount = 5000;  // as if a malformed response slipped through
  craft.requirements.push_back(over);
  CHECK_EQ(craft.acceptedAmount(), static_cast<int32_t>(100));
  CHECK_EQ(craft.remainingAmount(), static_cast<int32_t>(0));
}

static void test_PendingCraft_partial_is_true_only_with_real_progress() {
  PendingCraft craft;
  RequirementReservation untouched;
  untouched.required_amount = 100;
  craft.requirements.push_back(untouched);
  // Zero requirements, zero progress: not partial, and not complete either.
  CHECK(!craft.partial());

  craft.requirements[0].recordAccepted(1);
  CHECK(craft.partial());
  CHECK(craft.isPartial());
  CHECK(!craft.fullyAccepted());

  craft.requirements[0].recordAccepted(99);
  CHECK(!craft.partial());  // complete, so never "partial"
  CHECK(craft.fullyAccepted());
}

static void test_PendingCraft_partial_needs_at_least_one_requirement() {
  const PendingCraft craft;
  CHECK(!craft.partial());
  CHECK(!craft.isPartial());
}

static void test_PendingCraft_expired_honours_expiry_tick() {
  PendingCraft craft;
  // expiry_tick 0 disables expiry entirely — an unset timer never expires.
  CHECK(!craft.expired(0));
  CHECK(!craft.expired(1000000));

  craft.expiry_tick = 500;
  CHECK(!craft.expired(499));
  CHECK(craft.expired(500));  // boundary is inclusive
  CHECK(craft.expired(501));
}

static void test_PendingCraft_retryDue_compares_against_next_retry_tick() {
  PendingCraft craft;
  craft.next_retry_tick = 200;
  CHECK(!craft.retryDue(199));
  CHECK(craft.retryDue(200));
  CHECK(craft.retryDue(201));
}

// OBSERVED DEVIATION: next_retry_tick defaults to 0 and retryDue is a bare
// `now >= next_retry_tick`, so a freshly constructed PendingCraft is
// immediately retry-due at tick 0. Retry backoff must set next_retry_tick
// explicitly to suppress an immediate retry.
static void test_PendingCraft_is_retry_due_immediately_when_unscheduled() {
  PendingCraft craft;
  CHECK(craft.retryDue(0));
  CHECK(craft.retryDue(1));
}

static void test_PendingCraft_retry_count_is_plain_state() {
  PendingCraft craft;
  craft.retry_count = 3;
  CHECK_EQ(craft.retry_count, static_cast<uint32_t>(3));
  craft.retry_count++;
  CHECK_EQ(craft.retry_count, static_cast<uint32_t>(4));
}

static void test_PendingCraft_clear_resets_identity_and_reservations() {
  PendingCraft craft;
  craft.recipe_id = "gtnh:steam_turbine";
  craft.owner_id = 11u;
  craft.entity_id = 22u;
  craft.request_id = 33u;
  craft.epoch = 44u;
  craft.retry_count = 5u;
  craft.expiry_tick = 600u;
  craft.next_retry_tick = 700u;
  RequirementReservation reservation;
  reservation.required_amount = 100;
  reservation.request_id = 88u;
  craft.requirements.push_back(reservation);

  craft.clear();

  CHECK_EQ(craft.recipe_id, std::string());
  CHECK_EQ(craft.owner_id, static_cast<uint64_t>(0));
  CHECK_EQ(craft.entity_id, static_cast<uint64_t>(0));
  CHECK_EQ(craft.request_id, static_cast<uint64_t>(0));
  CHECK_EQ(craft.epoch, static_cast<uint64_t>(0));
  CHECK_EQ(craft.retry_count, static_cast<uint32_t>(0));
  CHECK_EQ(craft.expiry_tick, static_cast<uint64_t>(0));
  CHECK_EQ(craft.next_retry_tick, static_cast<uint64_t>(0));
  CHECK_EQ(craft.requirements.size(), static_cast<size_t>(0));
  CHECK(!craft.fullyAccepted());
  CHECK(!craft.expired(999999));
  // ...and it is retry-due again, matching the unscheduled default.
  CHECK(craft.retryDue(0));
}

static void test_PendingCraft_is_reusable_after_clear() {
  PendingCraft craft;
  craft.requirements.push_back(RequirementReservation());
  craft.requirements[0].required_amount = 100;
  craft.clear();

  RequirementReservation fresh;
  fresh.requirement_id = 9u;
  fresh.request_id = 99u;
  fresh.required_amount = 10;
  fresh.recordAccepted(10);
  craft.requirements.push_back(fresh);
  CHECK(craft.fullyAccepted());
  CHECK_EQ(craft.requiredAmount(), static_cast<int32_t>(10));
  CHECK_EQ(craft.acceptedAmount(), static_cast<int32_t>(10));
  CHECK(craft.findReservationByRequestId(99u) != nullptr);
}

// OBSERVED: responses are correlated by request id, not arrival order, so a
// craft with several outstanding requirements can be filled in any order and
// still totals correctly.
static void test_PendingCraft_correlates_out_of_order_responses() {
  PendingCraft craft;
  for (uint32_t i = 0; i < 3; ++i) {
    RequirementReservation reservation;
    reservation.requirement_id = i;
    reservation.request_id = 100u + i;
    reservation.required_amount = 100;
    craft.requirements.push_back(reservation);
  }

  // Responses arrive for request 102, then 100, then 101.
  const uint64_t order[] = {102u, 100u, 101u};
  for (uint64_t request_id : order) {
    RequirementReservation* target = craft.findReservationByRequestId(request_id);
    CHECK(target != nullptr);
    if (target != nullptr) target->recordAccepted(100);
  }

  CHECK(craft.fullyAccepted());
  CHECK_EQ(craft.requiredAmount(), static_cast<int32_t>(300));
  CHECK_EQ(craft.acceptedAmount(), static_cast<int32_t>(300));
  CHECK_EQ(craft.remainingAmount(), static_cast<int32_t>(0));
  // Each reservation kept its own identity.
  for (size_t i = 0; i < craft.requirements.size(); ++i) {
    CHECK_EQ(craft.requirements[i].request_id, static_cast<uint64_t>(100 + i));
    CHECK_EQ(craft.requirements[i].accepted_amount, static_cast<int32_t>(100));
  }
}

// ===========================================================================
// gp-9p6 — RecipeProgress.h : RecipeProgress
// ===========================================================================

static void test_RecipeProgress_defaults_are_idle() {
  const RecipeProgress progress;
  CHECK_EQ(progress.recipe_id, std::string());
  CHECK_EQ(progress.remaining_ticks, static_cast<uint32_t>(0));
  CHECK_EQ(progress.is_processing, false);
  CHECK_EQ(progress.needs_output, false);
  CHECK(!progress.pending_craft.has_value());
}

static void test_RecipeProgress_is_processing_and_needs_output_are_independent() {
  RecipeProgress progress;
  progress.is_processing = true;
  progress.needs_output = true;
  CHECK(progress.is_processing);
  CHECK(progress.needs_output);

  // The pair encodes the three machine states independently: idle, running,
  // and finished-but-output-pending.
  progress.is_processing = false;
  CHECK(!progress.is_processing);
  CHECK(progress.needs_output);
}

static void test_RecipeProgress_remaining_ticks_counts_down_independently() {
  RecipeProgress progress;
  progress.recipe_id = "gtnh:lv_steam_turbine";
  progress.remaining_ticks = 200;
  CHECK_EQ(progress.recipe_id, std::string("gtnh:lv_steam_turbine"));
  for (int tick = 0; tick < 200; ++tick) {
    progress.remaining_ticks = (progress.remaining_ticks > 0)
                                   ? progress.remaining_ticks - 1
                                   : progress.remaining_ticks;
  }
  CHECK_EQ(progress.remaining_ticks, static_cast<uint32_t>(0));
  // The recipe id is not cleared by the component; the owning system does it.
  CHECK_EQ(progress.recipe_id, std::string("gtnh:lv_steam_turbine"));
}

static void test_RecipeProgress_clearPendingCraft_resets_the_optional() {
  RecipeProgress progress;
  PendingCraft craft;
  craft.recipe_id = "gtnh:reactor";
  craft.requirements.push_back(RequirementReservation());
  progress.pending_craft = craft;
  CHECK(progress.pending_craft.has_value());

  progress.clearPendingCraft();
  CHECK(!progress.pending_craft.has_value());

  // Clearing twice is harmless.
  progress.clearPendingCraft();
  CHECK(!progress.pending_craft.has_value());
  CHECK_EQ(progress.recipe_id, std::string());
  CHECK_EQ(progress.remaining_ticks, static_cast<uint32_t>(0));
  CHECK_EQ(progress.is_processing, false);
  CHECK_EQ(progress.needs_output, false);
}

static void test_RecipeProgress_empty_recipe_id_is_the_idle_marker() {
  RecipeProgress progress;
  progress.recipe_id = "gtnh:boiler";
  CHECK_NE(progress.recipe_id, std::string());
  progress.recipe_id.clear();
  CHECK_EQ(progress.recipe_id, std::string());
}

static void test_RecipeProgress_pending_craft_survives_value_semantics() {
  RecipeProgress original;
  PendingCraft craft;
  craft.recipe_id = "gtnh:reactor";
  RequirementReservation reservation;
  reservation.required_amount = 100;
  reservation.request_id = 5u;
  craft.requirements.push_back(reservation);
  original.pending_craft = craft;

  RecipeProgress copy = original;
  CHECK(copy.pending_craft.has_value());
  // The copy owns its own reservation vector.
  copy.pending_craft->requirements[0].recordAccepted(100);
  CHECK_EQ(original.pending_craft->requirements[0].accepted_amount, static_cast<int32_t>(0));
  CHECK(copy.pending_craft->fullyAccepted());
}

static void test_RecipeProgress_inputs_must_wait_for_full_acceptance() {
  // The contract the component encodes: is_processing may already be true, but
  // input items must not be consumed until pending_craft is fully accepted.
  RecipeProgress progress;
  progress.recipe_id = "gtnh:reactor";
  progress.is_processing = true;

  PendingCraft craft;
  RequirementReservation reservation;
  reservation.required_amount = 1000;
  reservation.request_id = 1u;
  craft.requirements.push_back(reservation);
  progress.pending_craft = craft;

  CHECK(!progress.pending_craft->fullyAccepted());
  CHECK(progress.pending_craft->remainingAmount() > 0);

  progress.pending_craft->findReservationByRequestId(1u)->recordAccepted(1000);
  CHECK(progress.pending_craft->fullyAccepted());
  CHECK_EQ(progress.pending_craft->remainingAmount(), static_cast<int32_t>(0));
}

static void test_ResourceKind_matches_the_shared_common_contract() {
  // RecipeProgress.h probes for common/ResourcePort.h and, when found, aliases
  // the canonical gtnh::common::ResourceKind. Assert the ordinals line up
  // either way so the fallback enum can never silently diverge.
  CHECK_EQ(static_cast<uint8_t>(ResourceKind::FLUID),
           static_cast<uint8_t>(gtnh::common::ResourceKind::FLUID));
  CHECK_EQ(static_cast<uint8_t>(ResourceKind::EU),
           static_cast<uint8_t>(gtnh::common::ResourceKind::EU));
  CHECK_EQ(static_cast<uint8_t>(ResourceKind::HU),
           static_cast<uint8_t>(gtnh::common::ResourceKind::HU));
  CHECK_EQ(static_cast<uint8_t>(ResourceKind::RU),
           static_cast<uint8_t>(gtnh::common::ResourceKind::RU));
  CHECK_EQ(static_cast<uint8_t>(ResourceKind::ITEM),
           static_cast<uint8_t>(gtnh::common::ResourceKind::ITEM));
  // In this build the common header is on the include path, so the alias is
  // the canonical type itself.
  CHECK((std::is_same<ResourceKind, gtnh::common::ResourceKind>::value));
}

static void test_ResourceKind_is_carryable_in_the_reservation() {
  RequirementReservation reservation;
  reservation.kind = ResourceKind::EU;
  CHECK_EQ(static_cast<uint8_t>(reservation.kind),
           static_cast<uint8_t>(gtnh::common::ResourceKind::EU));
  reservation.clear();
  CHECK_EQ(static_cast<uint8_t>(reservation.kind),
           static_cast<uint8_t>(gtnh::common::ResourceKind::FLUID));
}

} // namespace

int main() {
  // gp-933 EnergyType
  TEST(EnergyType_declared_ordinals);
  TEST(EnergyType_is_uint8_backed_scoped_enum);
  // gp-2bs EnergySource
  TEST(EnergySource_is_an_empty_tag);
  TEST(EnergySource_is_trivially_default_constructible);
  // gp-3l3 Block
  TEST(Block_defaults_are_zero);
  TEST(Block_full_constructor_stores_all_fields);
  TEST(Block_zero_mb_id_is_the_single_block_sentinel);
  TEST(Block_aggregate_initialisation_order_is_id_meta_mb);
  // gp-doj BiomeComponent
  TEST(Biome_explicit_constructor_stores_id);
  TEST(Biome_accepts_the_full_uint16_range);
  TEST(Biome_value_initialisation_zeroes_id);
  // gp-mb9 Position
  TEST(Position_defaults_are_origin);
  TEST(Position_constructor_stores_coordinates);
  TEST(Position_is_unsigned_so_negative_coords_wrap);
  // gp-w2h BatteryBufferComponent
  TEST(BatteryBuffer_is_zero_initialised_when_aggregated);
  TEST(BatteryBuffer_holds_the_documented_GTNH_tier_values);
  TEST(BatteryBuffer_tier_and_slots_are_narrow_types);
  TEST(BatteryBuffer_stores_energy_and_acceptance_independently);
  // gp-io8 EnergyStorage
  TEST(EnergyStorage_default_constructor_is_all_zero_ELECTRICITY);
  TEST(EnergyStorage_full_constructor_defaults_type_to_ELECTRICITY);
  TEST(EnergyStorage_full_constructor_with_type_sets_type);
  TEST(EnergyStorage_addEnergy_takes_whole_request_below_limits);
  TEST(EnergyStorage_addEnergy_clamps_to_maxInput);
  TEST(EnergyStorage_addEnergy_clamps_to_remaining_space);
  TEST(EnergyStorage_addEnergy_takes_the_tighter_of_space_and_maxInput);
  TEST(EnergyStorage_addEnergy_accepts_nothing_when_full);
  TEST(EnergyStorage_addEnergy_with_zero_maxInput_accepts_nothing);
  TEST(EnergyStorage_addEnergy_ignores_non_positive_requests);
  TEST(EnergyStorage_addEnergy_with_negative_maxInput_moves_energy_backwards);
  TEST(EnergyStorage_produceEnergy_bypasses_maxInput);
  TEST(EnergyStorage_produceEnergy_clamps_to_remaining_space);
  TEST(EnergyStorage_produceEnergy_on_full_buffer_accepts_nothing);
  TEST(EnergyStorage_produceEnergy_ignores_non_positive_requests);
  TEST(EnergyStorage_produceEnergy_survives_over_capacity_state);
  TEST(EnergyStorage_consumeEnergy_takes_whole_request_below_limits);
  TEST(EnergyStorage_consumeEnergy_clamps_to_available);
  TEST(EnergyStorage_consumeEnergy_clamps_to_maxOutput);
  TEST(EnergyStorage_consumeEnergy_with_zero_maxOutput_consumes_nothing);
  TEST(EnergyStorage_consumeEnergy_ignores_zero_requests);
  TEST(EnergyStorage_consumeEnergy_with_a_negative_request_credits_energy_back);
  TEST(EnergyStorage_consumeEnergy_can_overshoot_into_negative_current);
  TEST(EnergyStorage_consumeEnergy_on_negative_current_credits_energy_back);
  TEST(EnergyStorage_consumeEnergy_with_negative_maxOutput_credits_energy_back);
  TEST(EnergyStorage_isFull_and_isEmpty_boundaries);
  TEST(EnergyStorage_default_constructed_is_simultaneously_empty_and_full);
  TEST(EnergyStorage_mutators_are_not_const_members);
  TEST(FluidStorage_mutators_are_not_const_members);
  TEST(InventoryContainer_mutation_helpers_are_not_const_members);
  TEST(EnergyStorage_charge_then_discharge_round_trips);
  // gp-cwe FluidStorage
  TEST(FluidStorage_defaults_are_all_zero);
  TEST(FluidStorage_full_constructor_stores_all_fields);
  TEST(FluidStorage_isFull_and_isEmpty_boundaries);
  TEST(FluidStorage_default_constructed_is_empty_and_full);
  TEST(FluidStorage_addFluid_takes_whole_request_below_limits);
  TEST(FluidStorage_addFluid_clamps_to_maxInput);
  TEST(FluidStorage_addFluid_clamps_to_remaining_space);
  TEST(FluidStorage_addFluid_accepts_nothing_when_full);
  TEST(FluidStorage_addFluid_with_zero_maxInput_accepts_nothing);
  TEST(FluidStorage_addFluid_ignores_non_positive_requests);
  TEST(FluidStorage_addFluid_with_negative_maxInput_moves_fluid_backwards);
  TEST(FluidStorage_removeFluid_takes_whole_request_below_limits);
  TEST(FluidStorage_removeFluid_clamps_to_available);
  TEST(FluidStorage_removeFluid_clamps_to_maxOutput);
  TEST(FluidStorage_removeFluid_with_zero_maxOutput_removes_nothing);
  TEST(FluidStorage_removeFluid_ignores_zero_requests);
  TEST(FluidStorage_removeFluid_with_a_negative_request_credits_fluid_back);
  TEST(FluidStorage_removeFluid_with_negative_maxOutput_credits_fluid_back);
  TEST(FluidStorage_fill_and_drain_round_trips);
  TEST(FluidStorage_helpers_ignore_the_fluid_id);
  // gp-aoa SideConfig
  TEST(SideRole_declared_ordinals);
  TEST(DEFAULT_SIDE_CONFIG_is_all_ANY);
  TEST(sideRoleName_covers_every_declared_role);
  TEST(sideRoleName_falls_back_for_out_of_range_values);
  TEST(nextSideRole_INPUT_branches_on_available_resources);
  TEST(nextSideRole_OUTPUT_and_ANY_return_to_INPUT);
  TEST(nextSideRole_ENERGY_goes_to_ANY);
  TEST(nextSideRole_fluid_roles_cycle_between_themselves);
  TEST(nextSideRole_NONE_and_out_of_range_reset_to_ANY);
  TEST(nextSideRole_energy_and_fluid_machine_cannot_reach_fluid_from_INPUT);
  TEST(nextSideRole_plain_machine_two_cycles_between_input_and_output);
  // gp-ug4 MachineComponent
  TEST(MachineFaceRole_declared_ordinals);
  TEST(MachineComponent_defaults_are_all_zero);
  TEST(MachineComponent_full_constructor_stores_identity_and_position);
  TEST(MachineComponent_constructor_leaves_optional_fields_defaulted);
  TEST(MachineComponent_face_roles_round_trip);
  TEST(MachineComponent_face_accessors_bounds_check_to_six);
  TEST(MachineComponent_side_config_is_independently_addressable);
  TEST(MachineComponent_default_side_config_disagrees_with_DEFAULT_SIDE_CONFIG);
  TEST(MachineComponent_managed_externally_flag_is_a_plain_bool);
  // gp-i8f HatchSlot
  TEST(HatchSlot_defaults_match_the_observed_state);
  TEST(HatchSlot_kSlotsPerHatch_is_four_for_item_hatches_only);
  TEST(HatchSlot_kSlotsPerHatch_is_usable_in_constant_expressions);
  TEST(HatchSlot_hasItemSlots_tracks_kSlotsPerHatch);
  TEST(HatchSlot_absent_hatch_still_reports_its_geometry);
  TEST(HatchSlot_default_side_config_is_five);
  // gp-38d InventoryContainer
  TEST(InventorySlot_defaults_are_empty);
  TEST(InventorySlot_constructor_stores_all_fields);
  TEST(InventoryContainer_defaults_are_empty);
  TEST(InventoryContainer_constructor_grows_slots_to_slot_count);
  TEST(InventoryContainer_constructor_never_shrinks_slots);
  TEST(InventoryContainer_getSlot_out_of_range_returns_empty_slot);
  TEST(InventoryContainer_setSlot_grows_the_slot_vector);
  TEST(InventoryContainer_addItem_fills_the_first_empty_slot);
  TEST(InventoryContainer_addItem_stacks_into_a_matching_slot);
  TEST(InventoryContainer_addItem_reports_wrapped_leftover_when_not_full);
  TEST(InventoryContainer_addItem_overflow_clamps_to_64);
  TEST(InventoryContainer_addItem_to_a_full_stack_spills_nowhere);
  TEST(InventoryContainer_addItem_on_a_full_container_returns_everything);
  TEST(InventoryContainer_addItem_oversized_into_empty_slot_discards_excess);
  TEST(InventoryContainer_addItem_ignores_meta_when_choosing_a_stack);
  TEST(InventoryContainer_addItem_with_zero_count_also_reports_wrapped_leftover);
  TEST(InventoryContainer_addItem_of_item_id_zero_never_fills_a_slot);
  TEST(InventoryContainer_addItem_fills_successive_empty_slots);
  TEST(InventoryContainer_removeItem_rejects_out_of_range_index);
  TEST(InventoryContainer_removeItem_rejects_more_than_present);
  TEST(InventoryContainer_removeItem_partial_removal_keeps_the_slot);
  TEST(InventoryContainer_removeItem_erases_and_shifts_later_slots);
  TEST(InventoryContainer_removeItem_zero_count_is_a_no_op);
  TEST(InventoryContainer_removeItem_of_empty_slot_erases_it);
  TEST(InventoryContainer_full_insert_and_remove_cycle);
  TEST(InventoryContainer_entity_type_is_opaque_metadata);
  TEST(InventoryContainer_independent_copies_do_not_alias);
  // gp-kp4 MultiblockController
  TEST(MultiblockController_defaults_are_empty);
  TEST(MultiblockController_constructor_stores_anchor_and_pattern);
  TEST(MultiblockController_constructor_takes_a_copy_of_blocks);
  TEST(MultiblockController_is_anchored_at_its_own_position);
  TEST(MultiblockController_hatches_are_appended_after_construction);
  TEST(MultiblockController_zero_id_is_distinct_from_a_real_id);
  // gp-e8p HeatIntakeComponent
  TEST(HeatIntake_defaults_are_empty_HEAT_buffer);
  TEST(HeatIntake_ratio_of_an_empty_buffer_is_zero);
  TEST(HeatIntake_ratio_tracks_stored_over_capacity);
  TEST(HeatIntake_ratio_honours_a_custom_capacity);
  TEST(HeatIntake_ratio_is_zero_for_zero_capacity);
  TEST(HeatIntake_ratio_is_zero_for_negative_capacity);
  TEST(HeatIntake_ratio_is_unclamped_outside_zero_to_one);
  TEST(HeatIntake_input_type_is_not_energy_by_default);
  // gp-9pu SteamOutputComponent
  TEST(SteamOutput_defaults_are_empty_STEAM_buffer);
  TEST(SteamOutput_ratio_of_an_empty_buffer_is_zero);
  TEST(SteamOutput_ratio_tracks_stored_over_capacity);
  TEST(SteamOutput_ratio_honours_a_custom_capacity);
  TEST(SteamOutput_ratio_is_zero_for_zero_and_negative_capacity);
  TEST(SteamOutput_ratio_is_unclamped_outside_zero_to_one);
  TEST(SteamOutput_stores_fractional_steam_exactly);
  TEST(SteamOutput_matches_heat_buffer_shape_with_wider_types);
  // gp-9p6 RecipeProgress: RequirementReservation
  TEST(RequirementReservation_defaults_are_empty);
  TEST(ResourceReservation_is_an_alias_of_RequirementReservation);
  TEST(RequirementReservation_remainingAmount_tracks_the_gap);
  TEST(RequirementReservation_remainingAmount_clamps_at_zero);
  TEST(RequirementReservation_fullyAccepted_requires_a_positive_requirement);
  TEST(RequirementReservation_partial_requires_some_progress);
  TEST(RequirementReservation_recordAccepted_accumulates_responses);
  TEST(RequirementReservation_recordAccepted_clamps_to_remaining);
  TEST(RequirementReservation_recordAccepted_ignores_non_positive_responses);
  TEST(RequirementReservation_recordAccepted_ignores_zero_requirements);
  TEST(RequirementReservation_recordAccepted_ignores_negative_requirements);
  TEST(RequirementReservation_clear_resets_every_field);
  TEST(RequirementReservation_reuse_after_clear_starts_from_zero);
  // gp-9p6 RecipeProgress: PendingCraft
  TEST(PendingCraft_defaults_are_idle);
  TEST(PendingCraft_reservations_accessors_agree);
  TEST(PendingCraft_findReservationByRequestId_matches_exactly_one);
  TEST(PendingCraft_findReservationByRequestId_ignores_zero_request_ids);
  TEST(PendingCraft_findReservationByRequestId_on_empty_craft);
  TEST(PendingCraft_fullyAccepted_requires_at_least_one_requirement);
  TEST(PendingCraft_fullyAccepted_requires_every_requirement);
  TEST(PendingCraft_totals_sum_across_requirements);
  TEST(PendingCraft_totals_ignore_negative_requirements);
  TEST(PendingCraft_acceptedAmount_clamps_over_acceptance);
  TEST(PendingCraft_partial_is_true_only_with_real_progress);
  TEST(PendingCraft_partial_needs_at_least_one_requirement);
  TEST(PendingCraft_expired_honours_expiry_tick);
  TEST(PendingCraft_retryDue_compares_against_next_retry_tick);
  TEST(PendingCraft_is_retry_due_immediately_when_unscheduled);
  TEST(PendingCraft_retry_count_is_plain_state);
  TEST(PendingCraft_clear_resets_identity_and_reservations);
  TEST(PendingCraft_is_reusable_after_clear);
  TEST(PendingCraft_correlates_out_of_order_responses);
  // gp-9p6 RecipeProgress: RecipeProgress
  TEST(RecipeProgress_defaults_are_idle);
  TEST(RecipeProgress_is_processing_and_needs_output_are_independent);
  TEST(RecipeProgress_remaining_ticks_counts_down_independently);
  TEST(RecipeProgress_clearPendingCraft_resets_the_optional);
  TEST(RecipeProgress_empty_recipe_id_is_the_idle_marker);
  TEST(RecipeProgress_pending_craft_survives_value_semantics);
  TEST(RecipeProgress_inputs_must_wait_for_full_acceptance);
  TEST(ResourceKind_matches_the_shared_common_contract);
  TEST(ResourceKind_is_carryable_in_the_reservation);

  std::printf("\n%d tests, %d passed, %d failed\n", g_tests, g_passed, g_failed);
  return g_failed > 0 ? 1 : 0;
}
