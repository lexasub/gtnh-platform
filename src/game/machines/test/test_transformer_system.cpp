// TransformerSystem unit tests (issue gp-ghyk).
//
// Covers src/game/machines/TransformerSystem.cpp — the two transformer blocks
// that convert between GregTech voltage tiers.
//
// VIEW LIVENESS (checked before writing these tests):
//   reg_.view<Block, Position, TransformerComponent, EnergyStorage>()
// None of the four components is MultiblockController, and all four ARE
// emplaced for a real transformer block placed in the world:
//   * SimulationEngine.cpp:179 emplaces Position for every block entity,
//   * SimulationEngine.cpp:199 emplaces Block,
//   * SimulationEngine.cpp:219/259 emplace MachineComponent + EnergyStorage for
//     any block MachineRegistry::IsMachine() accepts.
// TransformerComponent is the only one a caller must attach; it is
// transformer-specific state, so attaching it is the intended contract, not a
// defect. Unlike ExplosionSystem (see gp-43vj) this system's view IS
// satisfiable at runtime.
//
// VOLTAGE LADDER (TransformerSystem.cpp: tierVoltage(t) == 4^t * 8):
//   tier 0 ULV   8 EU
//   tier 1 LV   32 EU
//   tier 2 MV  128 EU
//   tier 3 HV  512 EU
//   tier 4 EV 2048 EU
// A transformer moves energy in whole packets of the INPUT tier voltage, so
// every ratio below is an exact multiple: 4 (UV->EV), 4 (LV..HV..EV), 1
// (same tier). No conversion is ever rounded UP — see the step-up loss tests.
//
// SYSTEM CONTRACT, per tick, for each entity in the view:
//   0. skip unless isTransformer(block.id) — only "1110:110:0" (mv_hv) and
//      "1110:110:1" (hv_ev) are transformers
//   1. step-up (tf.stepUp == true):
//        packets = min(energy.current, maxInput) / inputVoltage
//        only if packets > 0 AND buffer < maxOutput * 4:
//          consume   = packets * inputVoltage          (all of it, always)
//          ratio     = outputVoltage / inputVoltage
//          outPackets= packets / ratio                 (integer division)
//          buffer   += outPackets * outputVoltage
//          if buffer >= outputVoltage: flush floor(buffer/outputVoltage)
//                                      whole packets back to the buffer
//   2. step-down (tf.stepUp == false):
//        packets = min(energy.current, maxInput) / inputVoltage
//        if packets > 0: energy.current += min(packets*inputVoltage, space)
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed"):
//   A. STEP-UP IS NOT ENERGY-CONSERVING FOR PARTIAL PACKET GROUPS. `consume`
//      debits the full packets*inputVoltage but only floor(packets/ratio)
//      packets are emitted, so a group of `ratio+1` input packets silently
//      destroys one input-voltage worth of EU. Pinned by
//      test_TransformerSystem_step_up_loses_the_remainder_packet.
//   B. STEP-DOWN IS NOT A DISTRIBUTION — IT CREATES ENERGY. The step-down
//      branch reads energy.current, then ADDS packets*inputVoltage back into
//      the same energy.current, with no transfer out and no source for the
//      added EU. Because the amount is recomputed from the buffer the same
//      branch just grew, the charge DOUBLES every tick until it hits
//      capacity. It also never consults outputTier or the ratio, so the
//      declared output voltage has no effect at all in this direction.
//      Pinned by test_TransformerSystem_step_down_doubles_energy and
//      test_TransformerSystem_step_down_compounds_every_tick.
//   C. The step-up buffer gate `tf.buffer < tf.maxOutput * 4` blocks
//      consumption too, not just emission: a full buffer stalls input
//      entirely. Pinned by test_TransformerSystem_full_buffer_blocks_input.
#include <cstdio>
#include <cstdint>
#include <memory>

#include <entt/entt.hpp>

#include <engine/registry/ItemId.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/Position.h>
#include <game/machines/TransformerComponent.h>
#include <game/machines/TransformerSystem.h>

// Project-wide unit-test harness (src/engine/net/test/test.h) — the repo has no
// GTest dependency, so this is the established convention for focused tests.
#include <engine/net/test/test.h>

#ifndef CHECK_EQ_INT
#define CHECK_EQ_INT(a, b, ...) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, ##__VA_ARGS__)
#endif

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
// Constants asserted by name, so a tier change shows up in the diff.
// ---------------------------------------------------------------------------

namespace {

// TransformerSystem.cpp: tierVoltage(tier) == 4^tier * 8.
constexpr int32_t kVoltUlv = 8;     // tier 0 — 4^0 * 8
constexpr int32_t kVoltLv = 32;     // tier 1 — 4^1 * 8
constexpr int32_t kVoltMv = 128;    // tier 2 — 4^2 * 8
constexpr int32_t kVoltHv = 512;    // tier 3 — 4^3 * 8
constexpr int32_t kVoltEv = 2048;   // tier 4 — 4^4 * 8

// The two block ids isTransformer() accepts.
constexpr uint16_t kTransformerMvHv = ItemId::pack("1110:110:0");
constexpr uint16_t kTransformerHvEv = ItemId::pack("1110:110:1");

// MV->HV and HV->EV are both a 4x step, so `ratio` is 4 in both cases.
constexpr int32_t kTierRatio = 4;

// Rotation energy buffer cap: tf.buffer < tf.maxOutput * 4.
constexpr int32_t kBufferMul = 4;

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// The system is constructed with a null publisher and a null pipe client:
// TransformerSystem never dereferences events_ (it is stored and unused) and
// guards every pipeClient_ call, so a null client exercises the pure
// state-transition path with no router, socket or wall-clock dependency.
struct Fixture {
    entt::registry reg;
    simcore::TransformerSystem sys{reg, nullptr, nullptr};
};

static entt::entity makeTransformer(entt::registry& reg, uint32_t x, uint32_t y, uint32_t z,
                                    uint16_t block_id, uint8_t in_tier, uint8_t out_tier,
                                    bool step_up, int32_t buffer, int32_t current,
                                    int32_t capacity, int32_t max_in, int32_t max_out) {
    auto ent = reg.create();
    reg.emplace<simcore::Block>(ent, block_id, 0, 0);
    reg.emplace<simcore::Position>(ent, x, y, z);
    reg.emplace<simcore::TransformerComponent>(ent, in_tier, out_tier, step_up, buffer,
                                               max_in, max_out);
    reg.emplace<simcore::EnergyStorage>(ent, capacity, current, max_in, max_out, 1);
    return ent;
}

// MV->HV step-up, the common case, with generous headroom.
static entt::entity makeMvHvStepUp(entt::registry& reg, int32_t current, int32_t buffer = 0,
                                   int32_t capacity = 100000, int32_t max_in = 100000,
                                   int32_t max_out = 100000) {
    return makeTransformer(reg, 10, 64, 20, kTransformerMvHv, /*in_tier=*/2, /*out_tier=*/3,
                           /*step_up=*/true, buffer, current, capacity, max_in, max_out);
}

static auto& tf(entt::registry& reg, entt::entity ent) {
    return reg.get<simcore::TransformerComponent>(ent);
}
static auto& energy(entt::registry& reg, entt::entity ent) {
    return reg.get<simcore::EnergyStorage>(ent);
}

// ---------------------------------------------------------------------------
// isTransformer: the block-id gate
// ---------------------------------------------------------------------------

static void test_TransformerSystem_isTransformer_accepts_both_tier_blocks() {
    CHECK(simcore::TransformerSystem::isTransformer(kTransformerMvHv),
          "1110:110:0 (transformer_mv_hv) is a transformer");
    CHECK(simcore::TransformerSystem::isTransformer(kTransformerHvEv),
          "1110:110:1 (transformer_hv_ev) is a transformer");
    CHECK_EQ_INT(kTransformerMvHv != kTransformerHvEv, true,
                 "the two transformer block ids are distinct");
}

static void test_TransformerSystem_isTransformer_rejects_everything_else() {
    CHECK(!simcore::TransformerSystem::isTransformer(0), "air is not a transformer");
    CHECK(!simcore::TransformerSystem::isTransformer(1), "base block 1 is not a transformer");
    // Neighbouring payloads in the same "1110:110" sub-prefix are NOT accepted.
    CHECK(!simcore::TransformerSystem::isTransformer(ItemId::pack("1110:110:2")),
          "1110:110:2 is not a transformer");
    CHECK(!simcore::TransformerSystem::isTransformer(ItemId::pack("1110:110:3")),
          "1110:110:3 is not a transformer");
    // Nor is any other machine block, including the ones other systems own.
    CHECK(!simcore::TransformerSystem::isTransformer(ItemId::pack("1110:010:44")),
          "steam_turbine is not a transformer");
    CHECK(!simcore::TransformerSystem::isTransformer(ItemId::pack("1110:100:1")),
          "rotare_generator is not a transformer");
    CHECK(!simcore::TransformerSystem::isTransformer(ItemId::pack("1110:110")),
          "the bare sub-prefix with payload 0 is not a transformer");
}

static void test_TransformerSystem_non_transformer_block_is_skipped() {
    Fixture f;
    // Correct component set, correct transformer tiers — but a non-transformer
    // block id. The id gate is evaluated before any energy moves.
    auto ent = makeTransformer(f.reg, 1, 2, 3, ItemId::pack("1110:110:2"), 2, 3, true, 0,
                               kVoltMv * kTierRatio, 100000, 100000, 100000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, kVoltMv * kTierRatio,
                 "a non-transformer block is not drained");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "a non-transformer block buffers nothing");
}

static void test_TransformerSystem_missing_transformer_component_is_outside_the_view() {
    entt::registry reg;
    simcore::TransformerSystem sys(reg, nullptr, nullptr);
    auto ent = reg.create();
    reg.emplace<simcore::Block>(ent, kTransformerMvHv, 0, 0);
    reg.emplace<simcore::Position>(ent, 5, 6, 7);
    reg.emplace<simcore::EnergyStorage>(ent, 100000, kVoltMv * kTierRatio, 100000, 100000, 1);
    // No TransformerComponent: the 4-type view does not match.

    for (int i = 0; i < 5; ++i) sys.tick(0.05f);
    CHECK_EQ_INT(reg.get<simcore::EnergyStorage>(ent).current, kVoltMv * kTierRatio,
                 "without TransformerComponent the entity is outside the view");
}

static void test_TransformerSystem_missing_energy_storage_is_outside_the_view() {
    entt::registry reg;
    simcore::TransformerSystem sys(reg, nullptr, nullptr);
    auto ent = reg.create();
    reg.emplace<simcore::Block>(ent, kTransformerMvHv, 0, 0);
    reg.emplace<simcore::Position>(ent, 5, 6, 7);
    reg.emplace<simcore::TransformerComponent>(ent, 2, 3, true, 0, 100000, 100000);
    // No EnergyStorage.

    for (int i = 0; i < 5; ++i) sys.tick(0.05f);
    CHECK_EQ_INT(reg.get<simcore::TransformerComponent>(ent).buffer, 0,
                 "without EnergyStorage the entity is outside the view");
}

static void test_TransformerSystem_empty_registry_is_a_noop() {
    Fixture f;
    f.sys.tick(0.05f);
    f.sys.tick(1.0f);
    CHECK_EQ_INT(f.reg.storage<entt::entity>().size(), size_t(0), "no entities -> no work");
}

// ---------------------------------------------------------------------------
// The voltage ladder: a transformer only ever moves whole input-tier packets
// ---------------------------------------------------------------------------

static void test_TransformerSystem_input_quantum_is_the_input_tier_voltage() {
    // One tick at each tier with exactly one input packet available. The
    // transformer must consume precisely tierVoltage(tier) EU — never less, so
    // sub-packet charge never trickles through as fractional packets.
    struct Row { int tier; int32_t volt; bool step_up; uint16_t block; uint8_t out_tier; };
    const Row rows[] = {
        {0, kVoltUlv, true,  kTransformerMvHv, 1},
        {1, kVoltLv,  true,  kTransformerMvHv, 2},
        {2, kVoltMv,  true,  kTransformerMvHv, 3},
        {3, kVoltHv,  true,  kTransformerHvEv, 4},
        {4, kVoltEv,  true,  kTransformerHvEv, 4},
    };
    for (const auto& r : rows) {
        entt::registry reg;
        simcore::TransformerSystem sys(reg, nullptr, nullptr);
        auto ent = makeTransformer(reg, 0, 0, 0, r.block, static_cast<uint8_t>(r.tier),
                                   r.out_tier, r.step_up, 0, r.volt, 1000000, 1000000, 1000000);
        sys.tick(0.05f);
        CHECK_EQ_INT(energy(reg, ent).current, 0,
                     "exactly one input packet is consumed at the tier voltage");
    }
}

static void test_TransformerSystem_ladder_doubles_by_four_each_tier() {
    // The ladder is 4^t * 8, so each tier is exactly 4x the previous one.
    CHECK_EQ_INT(kVoltLv / kVoltUlv, kTierRatio, "LV is 4x ULV");
    CHECK_EQ_INT(kVoltMv / kVoltLv, kTierRatio, "MV is 4x LV");
    CHECK_EQ_INT(kVoltHv / kVoltMv, kTierRatio, "HV is 4x MV");
    CHECK_EQ_INT(kVoltEv / kVoltHv, kTierRatio, "EV is 4x HV");
    CHECK_EQ_INT(kVoltUlv, 8, "ULV base voltage is 8 EU");
}

static void test_TransformerSystem_sub_packet_charge_never_moves() {
    // A transformer charged with less than one input packet must do nothing:
    // there is no fractional-packet path, and the leftover must stay put
    // rather than being silently rounded up to a whole packet.
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, kVoltMv - 1);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, kVoltMv - 1,
                 "MV-1 EU is below one MV packet and is left untouched");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "no MV output packet is emitted");
}

static void test_TransformerSystem_zero_charge_never_moves() {
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, 0);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "an empty transformer stays empty");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "an empty transformer buffers nothing");
}

static void test_TransformerSystem_max_input_caps_the_per_tick_debit() {
    // energy.current holds far more than maxInput: only maxInput may be read
    // per tick, so the observed debit is maxInput rounded down to packets.
    Fixture f;
    const int32_t max_in = kVoltMv * kTierRatio;  // exactly 4 MV packets
    auto ent = makeMvHvStepUp(f.reg, 1000000, 0, 2000000, max_in, 1000000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 1000000 - max_in,
                 "per-tick debit is capped at maxInput");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "the 4 MV packets flushed as 1 HV packet");
}

static void test_TransformerSystem_max_input_below_one_packet_blocks_transfer() {
    // maxInput smaller than a single packet: nothing can ever be transferred.
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, 1000000, 0, 2000000, kVoltMv - 1, 1000000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 1000000, "maxInput below one packet transfers nothing");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "no output is produced");
}

// ---------------------------------------------------------------------------
// Step-up: the exact (lossless) conversions
// ---------------------------------------------------------------------------

static void test_TransformerSystem_step_up_mv_hv_is_exact() {
    // 4 MV packets (4 * 128 = 512 EU) in, ratio 4 -> 1 HV packet (512 EU) out.
    // Lossless: the transformer neither creates nor destroys EU here.
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, kVoltMv * kTierRatio);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "4 MV packets are consumed");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "the HV packet is flushed, leaving no buffer");
    // Conserved: in == out.
    CHECK_EQ_INT(kVoltMv * kTierRatio, kVoltHv, "4 MV packets equal exactly 1 HV packet");
}

static void test_TransformerSystem_step_up_hv_ev_is_exact() {
    Fixture f;
    auto ent = makeTransformer(f.reg, 3, 3, 3, kTransformerHvEv, /*in_tier=*/3, /*out_tier=*/4,
                               /*step_up=*/true, 0, kVoltHv * kTierRatio, 1000000, 1000000, 1000000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "4 HV packets are consumed");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "the EV packet is flushed");
    CHECK_EQ_INT(kVoltHv * kTierRatio, kVoltEv, "4 HV packets equal exactly 1 EV packet");
}

static void test_TransformerSystem_step_up_uv_to_ev_is_exact() {
    // Every documented tier pair in one table: ULV->EV is a 256x step
    // (4^4), and 256 ULV packets in produce exactly 1 EV packet out.
    Fixture f;
    const int32_t packets = 256;
    auto ent = makeTransformer(f.reg, 4, 4, 4, kTransformerHvEv, /*in_tier=*/0, /*out_tier=*/4,
                               /*step_up=*/true, 0, kVoltUlv * packets, 1000000, 1000000, 1000000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "256 ULV packets are consumed");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "the EV packet is flushed with no remainder");
    CHECK_EQ_INT(kVoltUlv * packets, kVoltEv, "256 ULV packets equal exactly 1 EV packet");
}

static void test_TransformerSystem_step_up_same_tier_is_identity() {
    // ratio 1: an in-tier transformer converts packets to packets with no loss.
    Fixture f;
    auto ent = makeTransformer(f.reg, 5, 5, 5, kTransformerMvHv, /*in_tier=*/2, /*out_tier=*/2,
                               /*step_up=*/true, 0, kVoltMv * 7, 1000000, 1000000, 1000000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "7 MV packets are consumed");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "all 7 packets flush as same-tier packets");
}

// ---------------------------------------------------------------------------
// FINDING A: step-up silently destroys the remainder packet
// ---------------------------------------------------------------------------

static void test_TransformerSystem_step_up_loses_the_remainder_packet() {
    // `consume` debits packets*inputVoltage but only floor(packets/ratio)
    // packets are emitted, so 5 MV packets (640 EU) in yields 1 HV packet
    // (512 EU) and 128 EU vanishes. Asserted exactly: this is the observed
    // behaviour, not an endorsement of it.
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, kVoltMv * (kTierRatio + 1));
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "all 5 MV packets (640 EU) are debited");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "only 1 HV packet (512 EU) leaves the buffer");
    CHECK_EQ_INT(kVoltMv * (kTierRatio + 1) - kVoltHv, kVoltMv,
                 "one input packet's worth of EU is destroyed per incomplete group");
}

static void test_TransformerSystem_step_up_loss_accumulates_across_ticks() {
    // Feeding 5 packets a tick for 10 ticks destroys 10 * 128 = 1280 EU and
    // emits 10 * 512 = 5120 EU — a 20% loss, pinned tick by tick.
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, kVoltMv * (kTierRatio + 1));
    for (int i = 0; i < 10; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "all 50 MV packets are consumed");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "10 HV packets are emitted, nothing stranded");
    // 50 packets * 128 = 6400 EU debited, 10 * 512 = 5120 EU emitted.
    CHECK_EQ_INT(kVoltMv * 50 - kVoltHv * 10, kVoltMv * 10,
                 "steady-state step-up loses exactly 20% of the energy it consumes");
}

// ---------------------------------------------------------------------------
// FINDING C: a full rotation buffer blocks input, not just output
// ---------------------------------------------------------------------------

static void test_TransformerSystem_full_buffer_blocks_input() {
    // The guard is `packets > 0 && buffer < maxOutput * 4`, so a full buffer
    // stops the transformer from debiting its input buffer at all. Under
    // normal operation the buffer is drained to < outputVoltage every tick,
    // so this only bites when there is nowhere to send the output.
    Fixture f;
    const int32_t max_out = 128;
    const int32_t buffer_cap = max_out * kBufferMul;  // 512
    auto ent = makeMvHvStepUp(f.reg, 100000, buffer_cap, 200000, 100000, max_out);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 100000,
                 "a buffer at maxOutput*4 blocks the input debit entirely");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, buffer_cap, "the buffer is left untouched");
}

static void test_TransformerSystem_buffer_below_cap_accepts_input_again() {
    Fixture f;
    const int32_t max_out = 128;
    const int32_t buffer_cap = max_out * kBufferMul;  // 512
    // One EU below the cap: the input debit resumes. The 4 MV packets add 512
    // to a buffer of 511, giving 1023 — the flush emits floor(1023/512) = 1
    // HV packet and strands the original 511, which is under one packet.
    auto ent = makeMvHvStepUp(f.reg, kVoltMv * kTierRatio, buffer_cap - 1, 200000, 100000, max_out);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "input is accepted once the buffer has room");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, buffer_cap - 1,
                 "one HV packet flushes out and the sub-packet remainder stays");
}

static void test_TransformerSystem_partial_buffer_keeps_the_remainder() {
    // A pre-loaded buffer below the cap still receives a whole input group; the
    // flush emits floor(buffer/outputVoltage) packets and keeps the rest.
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, kVoltMv * kTierRatio, /*buffer=*/100);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0, "the 4 MV packets are consumed");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 100,
                 "100 + 512 = 612 EU flushes 1 HV packet and strands the 100");
}

// ---------------------------------------------------------------------------
// FINDING B: step-down creates energy instead of distributing it
// ---------------------------------------------------------------------------

static void test_TransformerSystem_step_down_doubles_energy() {
    // The step-down branch reads packets out of energy.current and then ADDS
    // packets*inputVoltage straight back into the same buffer, with no
    // transfer anywhere. Charged with 1 MV packet it ends up with 2.
    Fixture f;
    auto ent = makeTransformer(f.reg, 6, 6, 6, kTransformerMvHv, /*in_tier=*/2, /*out_tier=*/3,
                               /*step_up=*/false, 0, kVoltMv, 100000, 100000, 100000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, kVoltMv * 2,
                 "step-down adds packets*inputVoltage back on top of the existing charge");
    CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "the step-down branch never touches the buffer");
}

static void test_TransformerSystem_step_down_compounds_every_tick() {
    // The addition is computed from the buffer the same branch just grew, so
    // the charge DOUBLES each tick rather than creeping up by a fixed packet:
    // 128 -> 256 -> 512 -> 1024 -> 2048 -> 4096.
    Fixture f;
    auto ent = makeTransformer(f.reg, 7, 7, 7, kTransformerMvHv, 2, 3, /*step_up=*/false, 0,
                               kVoltMv, 100000, 100000, 100000);
    int32_t expected = kVoltMv;
    for (int i = 0; i < 5; ++i) {
        f.sys.tick(0.05f);
        expected *= 2;
        CHECK_EQ_INT(energy(f.reg, ent).current, expected,
                     "step-down doubles the input buffer every tick");
    }
}

static void test_TransformerSystem_step_down_ignores_the_output_tier() {
    // outputTier and the ratio play no part in the step-down branch: the same
    // input produces the same growth whether the declared output is LV or EV.
    int32_t grown[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        entt::registry reg;
        simcore::TransformerSystem sys(reg, nullptr, nullptr);
        const uint8_t out_tier = (i == 0) ? 1 : 4;  // LV vs EV
        auto ent = makeTransformer(reg, 8, 8, 8, kTransformerMvHv, 2, out_tier,
                                   /*step_up=*/false, 0, kVoltMv * 10, 100000, 100000, 100000);
        sys.tick(0.05f);
        grown[i] = energy(reg, ent).current;
    }
    CHECK_EQ_INT(grown[0], grown[1],
                 "step-down output is identical for LV and EV declared output tiers");
}

static void test_TransformerSystem_step_down_below_one_packet_is_inert() {
    Fixture f;
    auto ent = makeTransformer(f.reg, 9, 9, 9, kTransformerMvHv, 2, 3, /*step_up=*/false, 0,
                               kVoltMv - 1, 100000, 100000, 100000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, kVoltMv - 1,
                 "step-down with under one input packet adds nothing");
}

static void test_TransformerSystem_step_down_from_empty_adds_nothing() {
    Fixture f;
    auto ent = makeTransformer(f.reg, 10, 10, 10, kTransformerMvHv, 2, 3, /*step_up=*/false, 0,
                               0, 100000, 100000, 100000);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, 0,
                 "an empty transformer cannot bootstrap EU from nothing");
}

static void test_TransformerSystem_step_down_clamps_at_capacity() {
    // The only thing bounding the step-down growth is the storage capacity:
    // space = capacity - current clamps the per-tick addition.
    Fixture f;
    const int32_t capacity = 1000;
    auto ent = makeTransformer(f.reg, 11, 11, 11, kTransformerMvHv, 2, 3, /*step_up=*/false, 0,
                               128, capacity, 100000, 100000);
    for (int i = 0; i < 50; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, capacity,
                 "step-down growth is bounded only by the storage capacity");
    // 8 ticks * 128 = 1024 to pass 872; the last is clamped by space.
    CHECK_GE(energy(f.reg, ent).current, capacity - kVoltMv,
             "the final tick is a partial add, not an overflow");
}

static void test_TransformerSystem_step_up_and_step_down_are_not_inverses() {
    // The issue asked for "step-up is the inverse of step-down". Read from the
    // code, it is not: step-up drains the buffer into tf.buffer and step-down
    // grows the same buffer. They are two unrelated code paths sharing a
    // component. Asserted so the divergence is visible rather than implied.
    Fixture up, down;
    auto a = makeTransformer(up.reg, 1, 1, 1, kTransformerMvHv, 2, 3, /*step_up=*/true, 0,
                             kVoltMv * kTierRatio, 100000, 100000, 100000);
    auto b = makeTransformer(down.reg, 1, 1, 1, kTransformerMvHv, 2, 3, /*step_up=*/false, 0,
                             kVoltMv * kTierRatio, 100000, 100000, 100000);
    up.sys.tick(0.05f);
    down.sys.tick(0.05f);

    CHECK_EQ_INT(energy(up.reg, a).current, 0, "step-up empties the input buffer");
    CHECK_EQ_INT(energy(down.reg, b).current, kVoltMv * kTierRatio * 2,
                 "step-down doubles the input buffer instead of emptying it");
    CHECK_NE(energy(up.reg, a).current, energy(down.reg, b).current,
             "step-up and step-down are not inverses of one another");
}

// ---------------------------------------------------------------------------
// Multi-entity and dt behaviour
// ---------------------------------------------------------------------------

static void test_TransformerSystem_each_transformer_is_updated_independently() {
    Fixture f;
    auto a = makeMvHvStepUp(f.reg, kVoltMv * kTierRatio);
    auto b = makeTransformer(f.reg, 2, 2, 2, kTransformerHvEv, 3, 4, true, 0,
                             kVoltHv * kTierRatio, 100000, 100000, 100000);
    // A non-transformer and an out-of-view entity that must both be ignored.
    auto noise = makeTransformer(f.reg, 3, 3, 3, ItemId::pack("1110:110:9"), 2, 3, true, 0,
                                 100000, 200000, 100000, 100000);
    auto ghost = f.reg.create();
    f.reg.emplace<simcore::Block>(ghost, kTransformerMvHv, 0, 0);
    f.reg.emplace<simcore::Position>(ghost, 4, 4, 4);
    f.reg.emplace<simcore::EnergyStorage>(ghost, 100000, 100000, 100000, 100000, 1);

    f.sys.tick(0.05f);

    CHECK_EQ_INT(energy(f.reg, a).current, 0, "MV->HV transformer ran");
    CHECK_EQ_INT(energy(f.reg, b).current, 0, "HV->EV transformer ran");
    CHECK_EQ_INT(energy(f.reg, noise).current, 100000, "the non-transformer is untouched");
    CHECK_EQ_INT(energy(f.reg, ghost).current, 100000, "the out-of-view entity is untouched");
}

static void test_TransformerSystem_dt_is_ignored() {
    // The system has no time integration: dt must not change the outcome.
    for (float dt : {0.0f, 0.05f, 1.0f, 100.0f}) {
        Fixture f;
        auto ent = makeMvHvStepUp(f.reg, kVoltMv * kTierRatio);
        f.sys.tick(dt);
        CHECK_EQ_INT(energy(f.reg, ent).current, 0, "step-up is independent of dt");
        CHECK_EQ_INT(tf(f.reg, ent).buffer, 0, "the HV packet flushed regardless of dt");
    }
}

static void test_TransformerSystem_repeated_ticks_are_idempotent_once_drained() {
    Fixture f;
    auto ent = makeMvHvStepUp(f.reg, kVoltMv * kTierRatio);
    f.sys.tick(0.05f);
    const int32_t after_first = energy(f.reg, ent).current;
    for (int i = 0; i < 10; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(energy(f.reg, ent).current, after_first,
                 "a drained transformer does not resume on later ticks");
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== transformer_system test suite ===\n\n");

    TEST(TransformerSystem_isTransformer_accepts_both_tier_blocks);
    TEST(TransformerSystem_isTransformer_rejects_everything_else);
    TEST(TransformerSystem_non_transformer_block_is_skipped);
    TEST(TransformerSystem_missing_transformer_component_is_outside_the_view);
    TEST(TransformerSystem_missing_energy_storage_is_outside_the_view);
    TEST(TransformerSystem_empty_registry_is_a_noop);

    TEST(TransformerSystem_input_quantum_is_the_input_tier_voltage);
    TEST(TransformerSystem_ladder_doubles_by_four_each_tier);
    TEST(TransformerSystem_sub_packet_charge_never_moves);
    TEST(TransformerSystem_zero_charge_never_moves);
    TEST(TransformerSystem_max_input_caps_the_per_tick_debit);
    TEST(TransformerSystem_max_input_below_one_packet_blocks_transfer);

    TEST(TransformerSystem_step_up_mv_hv_is_exact);
    TEST(TransformerSystem_step_up_hv_ev_is_exact);
    TEST(TransformerSystem_step_up_uv_to_ev_is_exact);
    TEST(TransformerSystem_step_up_same_tier_is_identity);
    TEST(TransformerSystem_step_up_loses_the_remainder_packet);
    TEST(TransformerSystem_step_up_loss_accumulates_across_ticks);
    TEST(TransformerSystem_full_buffer_blocks_input);
    TEST(TransformerSystem_buffer_below_cap_accepts_input_again);
    TEST(TransformerSystem_partial_buffer_keeps_the_remainder);

    TEST(TransformerSystem_step_down_doubles_energy);
    TEST(TransformerSystem_step_down_compounds_every_tick);
    TEST(TransformerSystem_step_down_ignores_the_output_tier);
    TEST(TransformerSystem_step_down_below_one_packet_is_inert);
    TEST(TransformerSystem_step_down_from_empty_adds_nothing);
    TEST(TransformerSystem_step_down_clamps_at_capacity);
    TEST(TransformerSystem_step_up_and_step_down_are_not_inverses);

    TEST(TransformerSystem_each_transformer_is_updated_independently);
    TEST(TransformerSystem_dt_is_ignored);
    TEST(TransformerSystem_repeated_ticks_are_idempotent_once_drained);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
