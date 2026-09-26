// SteamTurbineSystem unit tests (issue gp-q692).
//
// Covers src/game/machines/SteamTurbineSystem.cpp — steam consumption and EU
// generation. Steam is a core GTNH resource; the turbine is the machine that
// turns it into EU.
//
// VIEW LIVENESS (checked before writing these tests):
//   reg_.view<MachineComponent, SteamTurbineComponent>()
// MachineComponent IS emplaced by SimulationEngine.cpp:219 for any block
// MachineRegistry::IsMachine() accepts, and "1110:010:44" (steam_turbine) is
// registered in machines.yaml. But SteamTurbineComponent is emplaced NOWHERE
// in production code: `grep -rn SteamTurbineComponent src/` matches only
// SteamTurbineSystem.h/.cpp and the old simcored_test. SimulationEngine never
// creates it, so the second half of the view is unsatisfiable in a running
// server. FINDING: the system is DEAD AT RUNTIME for the same class of reason
// as ExplosionSystem (gp-43vj), except here the missing component is a
// dedicated one rather than MultiblockController. The view is still fully
// satisfiable in a test, and the existing simcored_test
// (test_SteamTurbineSystem_converts_steam_to_eu) already emplaces it by hand,
// so this file tests the real logic while recording the wiring gap.
//
// SYSTEM CONTRACT, per tick, for each entity in the view:
//   0. skip unless machine.machine_id == kBlockId ("1110:010:44")
//   1. lazily adopt the registry-resolved steam_item_id when it is 0
//   2. with a FluidClient and a resolved steam id: publish the steam node, and
//      when the tank is below capacity and no request is outstanding, request
//      min(steam_max_input, capacity - stored) and raise request_pending
//   3. eu_space     = eu_capacity - eu_stored
//      steam_to_use = min(steam_stored, eu_max_output,
//                         eu_space / max(1, eu_per_steam))
//      if steam_to_use > 0: stored -= steam_to_use; eu_stored += used * eu_per_steam
//   4. publish the EU node (if a PipeEnergyClient) and the block-entity update
//
// FINDINGS BAKED INTO THESE TESTS (asserted as observed, never "fixed"):
//   A. eu_max_output IS COMPARED AGAINST A STEAM COUNT, NOT AN EU AMOUNT. It
//      sits in a min() alongside steam_stored, so it caps how much STEAM burns
//      per tick, and the EU emitted is steam_to_use * eu_per_steam. The
//      per-tick EU output is therefore eu_max_output * eu_per_steam, which
//      for a high-ratio turbine is far above the "max output" the field name
//      promises: ratio 100 emits up to 3200 EU in one tick, not 32.
//      Pinned by test_SteamTurbineSystem_max_output_is_a_steam_count_not_an_eu_cap.
//   B. THE BUFFER CAN NEVER BE FILLED WITHIN eu_per_steam-1 OF CAPACITY. The
//      third min term is floor(eu_space / eu_per_steam), so the last partial
//      packet is unreachable: a 100 EU buffer at ratio 7 tops out at 98.
//      Pinned by test_SteamTurbineSystem_never_overshoots_eu_capacity.
//   C. eu_per_steam == 0 DESTROYS STEAM FOR NOTHING. max(1, 0) makes the whole
//      eu_space look available, so steam_to_use is the full 32-packet burn and
//      the gain is 32 * 0 = 0 EU. A misconfigured ratio silently eats steam.
//      Pinned by test_SteamTurbineSystem_zero_ratio_destroys_steam.
//   D. A FULL EU BUFFER HOLDS STEAM RATHER THAN BURNING IT, and eu_max_output
//      is a per-tick BURN limit, not a machinery output ceiling: an
//      unattended turbine keeps filling the buffer across ticks until capacity.
//      Pinned by test_SteamTurbineSystem_full_eu_buffer_holds_steam and
//      test_SteamTurbineSystem_output_is_not_limited_to_capacity.
#include <cstdio>
#include <cstdint>
#include <array>
#include <memory>
#include <vector>

#include <entt/entt.hpp>

#include "Network/FluidClient.h"
#include <engine/registry/ItemId.h>
#include <engine/sim/components/MachineComponent.h>
#include <game/machines/SteamTurbineComponent.h>
#include <game/machines/SteamTurbineSystem.h>

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
// Doubles
// ---------------------------------------------------------------------------

// The turbine's only publisher call is publishBlockEntityUpdate, so the double
// records just the fields the tests assert on.
struct PublisherEvent {
    int32_t x = 0, y = 0, z = 0;
    uint16_t machine_id = 0;
    uint32_t energy = 0;
    uint32_t energy_capacity = 0;
    double steam_current = -1.0;
    double steam_capacity = -1.0;
};

struct RecordingPublisher : simcore::IEventPublisher {
    std::vector<PublisherEvent> updates;

    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char*, uint32_t, uint8_t) override {}

    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t, uint8_t) override {}

    void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                  uint32_t, uint64_t) override {}

    void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z, uint16_t machine_type,
                                  const std::vector<uint8_t>&, float, uint32_t energy,
                                  EnergyType, uint32_t energy_capacity, int, float,
                                  const std::vector<HatchUpdateData>* = nullptr,
                                  double steam_current = -1.0,
                                  double steam_capacity = -1.0) override {
        updates.push_back({x, y, z, machine_type, energy, energy_capacity,
                           steam_current, steam_capacity});
    }

    void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                    uint16_t, uint8_t, uint16_t,
                                    const char*) override {}

    void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                          const std::array<uint8_t, 6>&) override {}

    void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t, uint16_t) override {}
    void publishMultiblockDestroyed(uint64_t) override {}

    void publishGridUpdate(int32_t, int32_t, int32_t,
                           const std::vector<RecipeManager::ItemStack>&) override {}
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

// A registry-resolved steam id, standing in for Registry::steamItemId().
// Any non-zero value works; the exact number is not the behaviour under test.
static constexpr uint16_t kSteamId = ItemId::pack("1111:11:1");

// Null energy/fluid clients: SteamTurbineSystem guards both
// (`if (energyClient_)`, `if (fluidClient_ && ...)`), so a null client
// exercises the conversion path with no router and no socket.
struct Fixture {
    entt::registry reg;
    std::shared_ptr<RecordingPublisher> events =
        std::make_shared<RecordingPublisher>();
    simcore::SteamTurbineSystem sys{reg, events, nullptr, nullptr, kSteamId};
};

static entt::entity makeTurbine(entt::registry& reg, uint16_t machine_id, uint32_t x,
                                 uint32_t y, uint32_t z, int32_t steam_stored,
                                 int32_t steam_capacity, int32_t steam_max_input,
                                 int32_t eu_per_steam, int32_t eu_stored, int32_t eu_capacity,
                                 int32_t eu_max_output) {
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, machine_id, 0, x, y, z, 1);
    simcore::SteamTurbineComponent t;
    t.steam_item_id = kSteamId;
    t.steam_stored = steam_stored;
    t.steam_capacity = steam_capacity;
    t.steam_max_input = steam_max_input;
    t.eu_per_steam = eu_per_steam;
    t.eu_stored = eu_stored;
    t.eu_capacity = eu_capacity;
    t.eu_max_output = eu_max_output;
    reg.emplace<simcore::SteamTurbineComponent>(ent, t);
    return ent;
}

// The stock steam_turbine values from machines.yaml (capacity 10000,
// max_output 32) crossed with the SteamTurbineComponent defaults
// (steam_capacity 1000, steam_max_input 32, eu_per_steam 1).
static entt::entity makeDefaultTurbine(entt::registry& reg, int32_t steam_stored) {
    return makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 300, 64, 300,
                       steam_stored, /*steam_capacity=*/1000, /*steam_max_input=*/32,
                       /*eu_per_steam=*/1, /*eu_stored=*/0, /*eu_capacity=*/10000,
                       /*eu_max_output=*/32);
}

static simcore::SteamTurbineComponent& turbine(entt::registry& reg, entt::entity ent) {
    return reg.get<simcore::SteamTurbineComponent>(ent);
}

// ---------------------------------------------------------------------------
// The block-id gate
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_kBlockId_matches_the_yaml_registration() {
    CHECK_EQ_INT(simcore::SteamTurbineSystem::kBlockId, ItemId::pack("1110:010:44"),
                 "the system targets the steam_turbine block registered in machines.yaml");
}

static void test_SteamTurbineSystem_non_turbine_machine_is_skipped() {
    Fixture f;
    // A full component set, charged with steam, but the wrong block id.
    auto ent = makeTurbine(f.reg, ItemId::pack("1110:110:0"), 1, 2, 3, /*steam_stored=*/100,
                           1000, 32, 1, /*eu_stored=*/0, 10000, 32);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 100, "a non-turbine machine burns no steam");
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 0, "a non-turbine machine produces no EU");
    CHECK_EQ_INT(f.events->updates.size(), size_t(0), "a non-turbine machine publishes nothing");
}

static void test_SteamTurbineSystem_missing_turbine_component_is_outside_the_view() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, nullptr, kSteamId);
    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, simcore::SteamTurbineSystem::kBlockId, 0,
                                           5, 6, 7, 1);
    // No SteamTurbineComponent: this is the state every real steam_turbine
    // entity is in, because nothing in production emplaces the component.
    for (int i = 0; i < 5; ++i) sys.tick(0.05f);
    CHECK_EQ_INT(events->updates.size(), size_t(0),
                 "a steam_turbine without SteamTurbineComponent is outside the view");
    CHECK(reg.valid(ent), "the entity is left intact");
}

static void test_SteamTurbineSystem_empty_registry_is_a_noop() {
    Fixture f;
    f.sys.tick(0.05f);
    f.sys.tick(1.0f);
    CHECK_EQ_INT(f.events->updates.size(), size_t(0), "no entities -> no events");
}

// ---------------------------------------------------------------------------
// The conversion rate: eu_per_steam
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_converts_one_eu_per_steam_by_default() {
    Fixture f;
    auto ent = makeDefaultTurbine(f.reg, /*steam_stored=*/32);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 0, "32 steam are consumed");
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 32, "32 steam produce 32 EU at eu_per_steam=1");
}

static void test_SteamTurbineSystem_honours_eu_per_steam_above_one() {
    // eu_per_steam 4 with 8 steam: 8 * 4 = 32 EU, below the eu_max_output cap
    // of 32 packets, so the whole stock is burned.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/8, 1000, 32, /*eu_per_steam=*/4,
                           /*eu_stored=*/0, /*eu_capacity=*/10000, /*eu_max_output=*/32);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 0, "all 8 steam are consumed");
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 32, "8 steam at 4 EU each produce 32 EU");
}

static void test_SteamTurbineSystem_eu_per_steam_of_one_hundred() {
    // FINDING A: eu_max_output (32) is min()'d against the STEAM count, not
    // the EU amount, so the burn is 32 steam even though each is worth 100 EU.
    // The tick emits 32 * 100 = 3200 EU, which is 100x the "max output" the
    // field name suggests.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/100, 1000, 32, /*eu_per_steam=*/100,
                           /*eu_stored=*/0, /*eu_capacity=*/10000, /*eu_max_output=*/32);
    f.sys.tick(0.05f);
    const auto& t = turbine(f.reg, ent);
    CHECK_EQ_INT(t.steam_stored, 100 - 32, "eu_max_output steam burn per tick, whatever the ratio");
    CHECK_EQ_INT(t.eu_stored, 32 * 100,
                 "32 steam at 100 EU each produce 3200 EU, not the advertised 32");
    CHECK_EQ_INT(32 * 100, 3200, "the real per-tick EU output is 100x eu_max_output here");
}

static void test_SteamTurbineSystem_eu_space_limits_the_burn() {
    // eu_per_steam 4, 200 EU of space, 500 steam available:
    // eu_space / eu_per_steam = 200/4 = 50 packets of steam, well above the
    // 32-packet eu_max_output cap, so the cap binds at 32.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/500, 1000, 32, /*eu_per_steam=*/4,
                           /*eu_stored=*/0, /*eu_capacity=*/10000, /*eu_max_output=*/32);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 32 * 4,
                 "32 packets * 4 EU = 128 EU in one tick");
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 500 - 32, "32 steam consumed");
}

static void test_SteamTurbineSystem_tight_eu_space_limits_the_burn() {
    // 3 EU of headroom at ratio 4: 3 / 4 = 0 packets, so nothing burns and the
    // buffer cannot move. This is the FINDING B residue: sub-packet headroom
    // is unreachable, so the last 3 EU of a 10000 EU buffer are unattainable.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/500, 1000, 32, /*eu_per_steam=*/4,
                           /*eu_stored=*/9997, /*eu_capacity=*/10000, /*eu_max_output=*/32);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 9997,
                 "3 EU of headroom at ratio 4 is less than one packet, so nothing burns");
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 500, "and no steam is consumed");
}

static void test_SteamTurbineSystem_eu_space_exactly_one_packet_burns() {
    // The boundary: 4 EU of headroom at ratio 4 is exactly one packet, so the
    // burn proceeds and lands exactly on capacity.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/500, 1000, 32, /*eu_per_steam=*/4,
                           /*eu_stored=*/9996, /*eu_capacity=*/10000, /*eu_max_output=*/32);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 10000, "exactly 4 EU of headroom burns 1 steam");
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 500 - 1, "one steam is consumed");
}

static void test_SteamTurbineSystem_zero_ratio_destroys_steam() {
    // FINDING C: max(1, 0) means the eu_space term divides by 1, so the
    // eu_space guard no longer protects anything. The full 32-packet burn
    // happens and the gain is 32 * 0 = 0 EU: steam is destroyed for nothing.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/100, 1000, 32, /*eu_per_steam=*/0,
                           /*eu_stored=*/0, /*eu_capacity=*/10000, /*eu_max_output=*/32);
    f.sys.tick(0.05f);
    const auto& t = turbine(f.reg, ent);
    CHECK_EQ_INT(t.steam_stored, 100 - 32, "a zero ratio still burns the full 32-packet cap");
    CHECK_EQ_INT(t.eu_stored, 0, "but produces no EU at all — the steam is destroyed");
}

// ---------------------------------------------------------------------------
// Missing steam supply: no negative EU, no phantom output
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_no_steam_produces_nothing() {
    Fixture f;
    auto ent = makeDefaultTurbine(f.reg, /*steam_stored=*/0);
    f.sys.tick(0.05f);
    const auto& t = turbine(f.reg, ent);
    CHECK_EQ_INT(t.eu_stored, 0, "an empty tank produces no EU");
    CHECK_EQ_INT(t.steam_stored, 0, "the tank is not driven negative");
    CHECK_EQ_INT(f.events->updates.size(), size_t(1),
                 "the turbine still publishes state, so the client sees a live machine");
}

static void test_SteamTurbineSystem_steam_never_goes_negative() {
    // eu_max_output far above the tank contents must not push steam_stored
    // below zero: min() takes the tank contents as the binding limit.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/3, 1000, 32, /*eu_per_steam=*/1,
                           /*eu_stored=*/0, /*eu_capacity=*/10000,
                           /*eu_max_output=*/9999);
    f.sys.tick(0.05f);
    const auto& t = turbine(f.reg, ent);
    CHECK_GE(t.steam_stored, 0, "steam_stored is clamped at zero, never negative");
    CHECK_EQ_INT(t.steam_stored, 0, "all 3 steam were consumed");
    CHECK_EQ_INT(t.eu_stored, 3, "and produced exactly 3 EU");
}

static void test_SteamTurbineSystem_many_idle_ticks_produce_no_eu() {
    Fixture f;
    auto ent = makeDefaultTurbine(f.reg, /*steam_stored=*/0);
    for (int i = 0; i < 200; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 0,
                 "200 ticks with no steam supply produce no EU at all");
}

// ---------------------------------------------------------------------------
// FINDING A: eu_max_output is a steam count, not an EU cap
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_max_output_is_a_steam_count_not_an_eu_cap() {
    // 100 steam in the tank, eu_max_output 32, ratio 1: the 32-packet burn
    // and the 32 EU produced happen to match, which is what makes the unit
    // mismatch invisible at the default ratio.
    Fixture f;
    auto ent = makeDefaultTurbine(f.reg, /*steam_stored=*/100);
    f.sys.tick(0.05f);
    const auto& t = turbine(f.reg, ent);
    CHECK_EQ_INT(t.steam_stored, 100 - 32, "only eu_max_output steam burn per tick");
    CHECK_EQ_INT(t.eu_stored, 32, "and at ratio 1 that is also 32 EU");
}

static void test_SteamTurbineSystem_burn_cap_applies_for_every_eu_per_steam() {
    // The cap is in steam units regardless of the ratio, so a high-ratio
    // turbine burns FEWER steam per tick, not more.
    int32_t burned[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        entt::registry reg;
        auto events = std::make_shared<RecordingPublisher>();
        const int32_t ratio = (i + 1) * 4;  // 4, 8, 12
        simcore::SteamTurbineSystem sys(reg, events, nullptr, nullptr, kSteamId);
        auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                               /*steam_stored=*/1000, 2000, 32, ratio, 0, 1000000, 32);
        sys.tick(0.05f);
        burned[i] = 1000 - reg.get<simcore::SteamTurbineComponent>(ent).steam_stored;
    }
    CHECK_EQ_INT(burned[0], 32, "eu_per_steam 4 burns the full 32-packet cap");
    CHECK_EQ_INT(burned[1], 32, "eu_per_steam 8 also burns 32 steam");
    CHECK_EQ_INT(burned[2], 32, "eu_per_steam 12 also burns 32 steam");
}

// ---------------------------------------------------------------------------
// FINDING D (part 1): eu_max_output is a per-tick burn limit, not a ceiling
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_output_is_not_limited_to_capacity() {
    // SteamTurbineComponent.h documents eu_max_output as "max EU output", but
    // the buffer keeps filling past it on later ticks: 100 steam over 10 ticks
    // at 32/tick yields 320 EU, not 32.
    Fixture f;
    auto ent = makeDefaultTurbine(f.reg, /*steam_stored=*/100);
    for (int i = 0; i < 4; ++i) f.sys.tick(0.05f);
    // Ticks 1-3 burn 32 each (96 EU); tick 4 burns the remaining 4.
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 100,
                 "all 100 steam convert over 4 ticks, not 32");
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 0, "the tank ends empty");
}

static void test_SteamTurbineSystem_fills_the_whole_eu_capacity() {
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/100000, /*steam_capacity=*/200000, 32,
                           /*eu_per_steam=*/1, /*eu_stored=*/0, /*eu_capacity=*/10000,
                           /*eu_max_output=*/32);
    // 10000 / 32 = 312.5 -> 313 ticks.
    for (int i = 0; i < 400; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 10000,
                 "the EU buffer fills to capacity and stops there");
}

static void test_SteamTurbineSystem_never_overshoots_eu_capacity() {
    // FINDING B: eu_per_steam 7 against a 100 EU capacity. The
    // floor(eu_space / eu_per_steam) term prevents the overshoot but leaves
    // the last 2 EU permanently unreachable.
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/100000, 200000, 32, /*eu_per_steam=*/7, 0,
                           /*eu_capacity=*/100, /*eu_max_output=*/32);
    for (int i = 0; i < 50; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 98,
                 "100/7 = 14 packets, so 98 EU fit and the last 2 are not squeezed in");
    CHECK_GE(turbine(f.reg, ent).eu_stored, 0, "eu_stored never goes negative");
}

// ---------------------------------------------------------------------------
// FINDING D (part 2): a full EU buffer holds steam rather than burning it
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_full_eu_buffer_holds_steam() {
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/100, 1000, 32, 1, /*eu_stored=*/10000,
                           /*eu_capacity=*/10000, /*eu_max_output=*/32);
    for (int i = 0; i < 20; ++i) f.sys.tick(0.05f);
    const auto& t = turbine(f.reg, ent);
    CHECK_EQ_INT(t.eu_stored, 10000, "a full EU buffer stays full");
    CHECK_EQ_INT(t.steam_stored, 100, "the full tank is held, not drained");
}

static void test_SteamTurbineSystem_resumes_when_eu_space_returns() {
    Fixture f;
    auto ent = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/100, 1000, 32, 1, /*eu_stored=*/10000,
                           /*eu_capacity=*/10000, 32);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 100, "nothing burns while the buffer is full");

    // Draining 64 EU (what a downstream consumer would do) re-opens the burn.
    turbine(f.reg, ent).eu_stored = 10000 - 64;
    f.sys.tick(0.05f);
    CHECK_EQ_INT(turbine(f.reg, ent).eu_stored, 10000 - 64 + 32,
                 "the next tick burns the newly available 32-packet window");
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 100 - 32, "and consumes 32 steam");
}

// ---------------------------------------------------------------------------
// The steam item id: lazy adoption and the fail-closed case
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_adopts_the_registry_steam_id_when_zero() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, nullptr, kSteamId);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/10, 1000, 32, 1, 0, 10000, 32);
    reg.get<simcore::SteamTurbineComponent>(ent).steam_item_id = 0;

    sys.tick(0.05f);

    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).steam_item_id, kSteamId,
                 "a zero steam_item_id is replaced with the registry-resolved id");
    // The conversion is independent of the id, so it still runs.
    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).eu_stored, 10,
                 "conversion runs regardless of the steam id");
}

static void test_SteamTurbineSystem_keeps_an_explicit_steam_id() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, nullptr, kSteamId);
    const uint16_t custom = ItemId::pack("1111:11:9");
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/10, 1000, 32, 1, 0, 10000, 32);
    reg.get<simcore::SteamTurbineComponent>(ent).steam_item_id = custom;

    sys.tick(0.05f);

    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).steam_item_id, custom,
                 "an explicitly set steam_item_id is not overwritten");
}

static void test_SteamTurbineSystem_zero_steam_id_still_converts() {
    // The steam id gates only the fluid-request path. A registry that failed
    // to resolve steam (id 0) does not stop the conversion, so a turbine
    // holding steam keeps making EU — it just never tops up.
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, nullptr, /*steam_item_id=*/0);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/10, 1000, 32, 1, 0, 10000, 32);
    reg.get<simcore::SteamTurbineComponent>(ent).steam_item_id = 0;

    sys.tick(0.05f);

    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).eu_stored, 10,
                 "conversion runs even with an unresolved steam id");
    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).steam_item_id, 0,
                 "the unresolved id is not replaced with 0's worth of a guess");
}

// ---------------------------------------------------------------------------
// onFluidConsumeResponse
// ---------------------------------------------------------------------------

// A real FluidClient double: both of its publishing methods are virtual, so
// the request path can be driven without a router. The base constructor takes
// a null router that the overrides never touch.
struct RecordingFluidClient : simcore::FluidClient {
    struct NodeUpdate {
        uint64_t node_id;
        int32_t x, y, z;
        uint32_t fluid_id;
        int32_t amount, capacity, max_input, max_output, tier;
        bool is_source, is_sink;
    };
    struct Request {
        uint64_t node_id;
        int32_t x, y, z;
        uint32_t fluid_id;
        int32_t amount;
    };

    std::vector<NodeUpdate> nodes;
    std::vector<Request> requests;

    RecordingFluidClient() : simcore::FluidClient(nullptr) {}

    void publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                           uint32_t fluid_id, int32_t amount, int32_t capacity,
                           int32_t max_input, int32_t max_output, int32_t tier,
                           bool is_source, bool is_sink,
                           const std::vector<uint64_t>&) override {
        nodes.push_back({node_id, x, y, z, fluid_id, amount, capacity, max_input,
                         max_output, tier, is_source, is_sink});
    }

    void sendFluidRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                          uint32_t fluid_id, int32_t amount) override {
        requests.push_back({node_id, x, y, z, fluid_id, amount});
    }
};

static void test_SteamTurbineSystem_consume_response_without_a_request_is_a_noop() {
    Fixture f;
    auto ent = makeDefaultTurbine(f.reg, /*steam_stored=*/0);
    f.sys.tick(0.05f);
    // No request is outstanding (the tick used a null FluidClient, so
    // request_pending was never set), so the response has nowhere to go.
    f.sys.onFluidConsumeResponse(500);
    CHECK_EQ_INT(turbine(f.reg, ent).steam_stored, 0,
                 "a response with no outstanding request adds no steam");
}

static void test_SteamTurbineSystem_requests_steam_up_to_the_input_quantum() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, kSteamId);
    // Empty tank: the node advertises capacity and requests a top-up.
    makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 10, 20, 30,
                /*steam_stored=*/0, /*steam_capacity=*/1000, /*steam_max_input=*/32, 1, 0,
                10000, 32);
    sys.tick(0.05f);

    CHECK_EQ_INT(fluid->nodes.size(), size_t(1), "the steam node is published every tick");
    CHECK_EQ_INT(fluid->requests.size(), size_t(1), "one steam request is issued on a refill");
    if (!fluid->requests.empty()) {
        CHECK_EQ_INT(int(fluid->requests[0].amount), 32,
                     "the request is steam_max_input, the declared per-delivery quantum");
        CHECK_EQ_INT(int(fluid->requests[0].fluid_id), kSteamId,
                     "the request carries the registry-resolved steam id");
        CHECK_EQ_INT(fluid->requests[0].x, 10, "the request targets the machine x");
        CHECK_EQ_INT(fluid->requests[0].y, 20, "the request targets the machine y");
        CHECK_EQ_INT(fluid->requests[0].z, 30, "the request targets the machine z");
    }
}

static void test_SteamTurbineSystem_only_one_request_is_outstanding() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, kSteamId);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/0, 1000, 32, 1, 0, 10000, 32);

    for (int i = 0; i < 10; ++i) sys.tick(0.05f);

    CHECK_EQ_INT(fluid->requests.size(), size_t(1),
                 "request_pending blocks a second request until the response arrives");
    CHECK(reg.get<simcore::SteamTurbineComponent>(ent).request_pending,
          "the pending flag is still raised after 10 ticks");
}

static void test_SteamTurbineSystem_response_credits_steam_and_clears_pending() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, kSteamId);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/0, 1000, 32, 1, 0, 10000, 32);
    sys.tick(0.05f);
    CHECK(reg.get<simcore::SteamTurbineComponent>(ent).request_pending,
          "precondition: a request is outstanding");

    sys.onFluidConsumeResponse(64);
    const auto& t = reg.get<simcore::SteamTurbineComponent>(ent);
    CHECK(!t.request_pending, "the response clears request_pending");
    // The burn for the tick already ran with an empty tank, so the credited
    // 64 steam is still in the tank when the response lands.
    CHECK_EQ_INT(t.steam_stored, 64, "the response credits the consumed amount to the tank");

    // With pending_entity_ cleared, the next tick is free to request again.
    sys.tick(0.05f);
    CHECK_EQ_INT(fluid->requests.size(), size_t(2), "a new request is issued after the response");
}

static void test_SteamTurbineSystem_response_cannot_overfill_the_tank() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, kSteamId);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/900, /*steam_capacity=*/1000, 32, 1, 0, 10000, 32);
    sys.tick(0.05f);
    // A short-fill that over-delivers relative to the remaining headroom.
    sys.onFluidConsumeResponse(5000);
    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).steam_stored, 1000,
                 "an over-delivering response is clamped to steam_capacity");
}

static void test_SteamTurbineSystem_zero_response_still_clears_pending() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, kSteamId);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/0, 1000, 32, 1, 0, 10000, 32);
    sys.tick(0.05f);

    // A short-fill of 0: pending is cleared so the machine can ask again, and
    // no steam is credited.
    sys.onFluidConsumeResponse(0);
    CHECK(!reg.get<simcore::SteamTurbineComponent>(ent).request_pending,
          "a zero response still releases the outstanding request");
    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).steam_stored, 0,
                 "a zero response credits no steam");
}

static void test_SteamTurbineSystem_full_tank_defers_the_request_by_one_tick() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, kSteamId);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/1000, /*steam_capacity=*/1000, 32, 1, 0, 10000, 32);

    // The refill check runs BEFORE the burn in the same tick, so a tank that is
    // exactly full on entry defers by one tick. The burn then takes 32, so the
    // next tick finds headroom and asks.
    sys.tick(0.05f);
    CHECK_EQ_INT(fluid->requests.size(), size_t(0),
                 "a tank that is exactly full defers the refill request");
    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).steam_stored, 1000 - 32,
                 "the burn still runs on the same tick, creating headroom");

    sys.tick(0.05f);
    CHECK_EQ_INT(fluid->requests.size(), size_t(1),
                 "the next tick finds headroom and requests a refill");
    CHECK_EQ_INT(fluid->nodes.size(), size_t(2), "the node is published every tick regardless");
}

static void test_SteamTurbineSystem_zero_steam_id_never_requests() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    // steam_item_id 0 fails the `turbine.steam_item_id != 0` guard, so the
    // whole fluid path is skipped — but the conversion still runs.
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, /*steam_item_id=*/0);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/100, 1000, 32, 1, 0, 10000, 32);
    reg.get<simcore::SteamTurbineComponent>(ent).steam_item_id = 0;
    sys.tick(0.05f);
    CHECK_EQ_INT(fluid->requests.size(), size_t(0), "an unresolved steam id requests nothing");
    CHECK_EQ_INT(fluid->nodes.size(), size_t(0), "and publishes no steam node");
    CHECK_EQ_INT(reg.get<simcore::SteamTurbineComponent>(ent).eu_stored, 32,
                 "but the conversion still runs on the steam already in the tank");
}

static void test_SteamTurbineSystem_response_on_a_destroyed_entity_is_safe() {
    entt::registry reg;
    auto events = std::make_shared<RecordingPublisher>();
    auto fluid = std::make_shared<RecordingFluidClient>();
    simcore::SteamTurbineSystem sys(reg, events, nullptr, fluid, kSteamId);
    auto ent = makeTurbine(reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1,
                           /*steam_stored=*/0, 1000, 32, 1, 0, 10000, 32);
    sys.tick(0.05f);
    reg.destroy(ent);
    // Must not dereference a dead entity handle.
    sys.onFluidConsumeResponse(100);
    CHECK(!reg.valid(ent), "the destroyed entity stays destroyed");
    CHECK_EQ_INT(events->updates.size(), size_t(1), "no extra event from the dead response");
}

// ---------------------------------------------------------------------------
// Published state
// ---------------------------------------------------------------------------

static void test_SteamTurbineSystem_publishes_eu_and_steam_state() {
    Fixture f;
    makeDefaultTurbine(f.reg, /*steam_stored=*/100);
    f.sys.tick(0.05f);
    CHECK_EQ_INT(f.events->updates.size(), size_t(1), "one block-entity update per tick");
    if (!f.events->updates.empty()) {
        const auto& u = f.events->updates[0];
        CHECK_EQ_INT(u.x, 300, "publishes the machine x");
        CHECK_EQ_INT(u.y, 64, "publishes the machine y");
        CHECK_EQ_INT(u.z, 300, "publishes the machine z");
        CHECK_EQ_INT(u.machine_id, simcore::SteamTurbineSystem::kBlockId,
                     "publishes the steam_turbine machine id");
        CHECK_EQ_INT(u.energy, 32, "publishes the post-burn EU stored");
        CHECK_EQ_INT(u.energy_capacity, 10000, "publishes the EU capacity");
        CHECK_EQ_INT(int(u.steam_current), 68, "publishes the post-burn steam stored");
        CHECK_EQ_INT(int(u.steam_capacity), 1000, "publishes the steam capacity");
    }
}

static void test_SteamTurbineSystem_publishes_every_tick() {
    Fixture f;
    makeDefaultTurbine(f.reg, /*steam_stored=*/100);
    for (int i = 0; i < 5; ++i) f.sys.tick(0.05f);
    CHECK_EQ_INT(f.events->updates.size(), size_t(5),
                 "the turbine republishes every tick (it is never rate-limited)");
}

static void test_SteamTurbineSystem_each_turbine_publishes_independently() {
    Fixture f;
    auto a = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 1, 1, 1, 10, 1000, 32,
                         1, 0, 10000, 32);
    auto b = makeTurbine(f.reg, simcore::SteamTurbineSystem::kBlockId, 2, 2, 2, 50, 1000, 32,
                         1, 0, 10000, 32);
    // A non-turbine in the same view must be left alone.
    auto c = makeTurbine(f.reg, ItemId::pack("1110:100:1"), 3, 3, 3, 99, 1000, 32, 1, 0, 10000,
                         32);

    f.sys.tick(0.05f);

    CHECK_EQ_INT(turbine(f.reg, a).eu_stored, 10, "the first turbine converted its steam");
    CHECK_EQ_INT(turbine(f.reg, b).eu_stored, 32, "the second turbine hit its 32-packet cap");
    CHECK_EQ_INT(turbine(f.reg, c).eu_stored, 0, "the non-turbine is untouched");
    CHECK_EQ_INT(f.events->updates.size(), size_t(2), "exactly the two turbines published");
}

static void test_SteamTurbineSystem_dt_is_ignored() {
    // No time integration anywhere in the system: dt must not matter.
    for (float dt : {0.0f, 0.05f, 1.0f, 100.0f}) {
        Fixture f;
        auto ent = makeDefaultTurbine(f.reg, /*steam_stored=*/100);
        f.sys.tick(dt);
        const auto& t = turbine(f.reg, ent);
        CHECK_EQ_INT(t.steam_stored, 68, "steam burn is independent of dt");
        CHECK_EQ_INT(t.eu_stored, 32, "EU production is independent of dt");
    }
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== steam_turbine_system test suite ===\n\n");

    TEST(SteamTurbineSystem_kBlockId_matches_the_yaml_registration);
    TEST(SteamTurbineSystem_non_turbine_machine_is_skipped);
    TEST(SteamTurbineSystem_missing_turbine_component_is_outside_the_view);
    TEST(SteamTurbineSystem_empty_registry_is_a_noop);

    TEST(SteamTurbineSystem_converts_one_eu_per_steam_by_default);
    TEST(SteamTurbineSystem_honours_eu_per_steam_above_one);
    TEST(SteamTurbineSystem_eu_per_steam_of_one_hundred);
    TEST(SteamTurbineSystem_eu_space_limits_the_burn);
    TEST(SteamTurbineSystem_tight_eu_space_limits_the_burn);
    TEST(SteamTurbineSystem_eu_space_exactly_one_packet_burns);
    TEST(SteamTurbineSystem_zero_ratio_destroys_steam);

    TEST(SteamTurbineSystem_no_steam_produces_nothing);
    TEST(SteamTurbineSystem_steam_never_goes_negative);
    TEST(SteamTurbineSystem_many_idle_ticks_produce_no_eu);

    TEST(SteamTurbineSystem_max_output_is_a_steam_count_not_an_eu_cap);
    TEST(SteamTurbineSystem_burn_cap_applies_for_every_eu_per_steam);

    TEST(SteamTurbineSystem_output_is_not_limited_to_capacity);
    TEST(SteamTurbineSystem_fills_the_whole_eu_capacity);
    TEST(SteamTurbineSystem_never_overshoots_eu_capacity);

    TEST(SteamTurbineSystem_full_eu_buffer_holds_steam);
    TEST(SteamTurbineSystem_resumes_when_eu_space_returns);

    TEST(SteamTurbineSystem_adopts_the_registry_steam_id_when_zero);
    TEST(SteamTurbineSystem_keeps_an_explicit_steam_id);
    TEST(SteamTurbineSystem_zero_steam_id_still_converts);

    TEST(SteamTurbineSystem_consume_response_without_a_request_is_a_noop);
    TEST(SteamTurbineSystem_requests_steam_up_to_the_input_quantum);
    TEST(SteamTurbineSystem_only_one_request_is_outstanding);
    TEST(SteamTurbineSystem_response_credits_steam_and_clears_pending);
    TEST(SteamTurbineSystem_response_cannot_overfill_the_tank);
    TEST(SteamTurbineSystem_zero_response_still_clears_pending);
    TEST(SteamTurbineSystem_full_tank_defers_the_request_by_one_tick);
    TEST(SteamTurbineSystem_zero_steam_id_never_requests);
    TEST(SteamTurbineSystem_response_on_a_destroyed_entity_is_safe);

    TEST(SteamTurbineSystem_publishes_eu_and_steam_state);
    TEST(SteamTurbineSystem_publishes_every_tick);
    TEST(SteamTurbineSystem_each_turbine_publishes_independently);
    TEST(SteamTurbineSystem_dt_is_ignored);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
