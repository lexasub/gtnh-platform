// CreativeFluidSystem unit tests.
//
// The two creative fluid blocks (machines.yaml block_id 1110:100:15
// creative_oil_generator and 1110:100:16 creative_water_generator) must
// advertise themselves on the fluid network as INFINITE SOURCES, each with its
// OWN fluid. The failure this suite exists to prevent is silent: if the block
// id -> fluid id mapping collapses to one constant, or the argument order to
// publishNodeUpdate slips by one, nothing errors and a machine is quietly fed
// the wrong fluid.
//
// Harness: the project's own CHECK/TEST macros (same shape as
// test_adjacency_transfer_system.cpp). The repo has NO GTest dependency — it is
// not in conanfile.txt and CI builds with -Werror, so a target linking it is
// silently unregistered. No test here creates a socket, a thread or a router.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <entt/entt.hpp>

#include <apps/simcore/Network/FluidClient.h>
#include <apps/simcore/Network/IEventPublisher.h>
#include <engine/registry/ItemId.h>
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/FluidStorage.h>
#include <engine/sim/components/MachineComponent.h>
#include <game/mining/CreativeFluidSystem.h>

// ---------------------------------------------------------------------------
// Project harness
// ---------------------------------------------------------------------------

int g_tests = 0, g_passed = 0, g_failed = 0;

void test_check(bool cond, const char* file, int line, const char* expr, const char* msg = nullptr) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}

#define CHECK(cond, ...)                                                        \
    test_check(!!(cond), __FILE__, __LINE__, #cond, ##__VA_ARGS__)

#define CHECK_EQ_I(a, b, ...)                                                   \
    test_check(static_cast<int64_t>(a) == static_cast<int64_t>(b), __FILE__,    \
               __LINE__, #a " == " #b, ##__VA_ARGS__)

#define CHECK_NE_I(a, b, ...)                                                   \
    test_check(static_cast<int64_t>(a) != static_cast<int64_t>(b), __FILE__,    \
               __LINE__, #a " != " #b, ##__VA_ARGS__)

namespace {

using simcore::CreativeFluidSystem;
using simcore::EnergyStorage;
using simcore::FluidStorage;
using simcore::MachineComponent;

constexpr float kDt = 0.05f;  // 20 Hz, the simcore tick rate

// Block ids from src/content/data/registry/machines.yaml.
constexpr uint16_t kOilGenerator   = ItemId::pack("1110:100:15");
constexpr uint16_t kWaterGenerator = ItemId::pack("1110:100:16");
// The creative ENERGY generator is a different block and must NOT be claimed.
constexpr uint16_t kEnergyGenerator = ItemId::pack("1110:100:0");
constexpr uint16_t kHeatFurnace      = ItemId::pack("1110:000:0");

// Fluid ids from src/content/data/registry/fluids.csv. Asserted as LITERALS so
// the test pins the wire value: the system derives them with ItemId::pack, and
// if the packing ever changed these two must change with it, visibly.
constexpr uint32_t kOilFluid   = 64570;  // 1111:11:58
constexpr uint32_t kWaterFluid = 64512;  // 1111:11:0

// ---------------------------------------------------------------------------
// Recording publisher: implements the full IEventPublisher seam, records what
// the system publishes, never touches a socket.
// ---------------------------------------------------------------------------

struct EntityUpdateRecord {
    int32_t x = 0, y = 0, z = 0;
    uint16_t machine_type = 0;
    uint32_t energy = 0;
    size_t inventory_bytes = 0;
};

class RecordingPublisher : public simcore::IEventPublisher {
public:
    std::vector<EntityUpdateRecord> updates;

    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char*, uint32_t, uint8_t) override {}
    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t, uint8_t) override {}
    void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                  uint32_t, uint64_t) override {}

    void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z,
                                  uint16_t machine_type,
                                  const std::vector<uint8_t>& inventory_data,
                                  float, uint32_t energy,
                                  EnergyType = EnergyType::ELECTRICITY,
                                  uint32_t = 0, int = -1, float = 0.0f,
                                  const std::vector<HatchUpdateData>* = nullptr,
                                  double = -1.0, double = -1.0) override {
        updates.push_back(EntityUpdateRecord{x, y, z, machine_type, energy,
                                             inventory_data.size()});
    }

    void publishMachineSlotResponse(int32_t, int32_t, int32_t, uint16_t, bool,
                                    uint16_t, uint8_t, uint16_t,
                                    const char*) override {}
    void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                          const std::array<uint8_t, 6>&) override {}
    void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t,
                                  uint16_t) override {}
    void publishMultiblockDestroyed(uint64_t) override {}
    void publishGridUpdate(int32_t, int32_t, int32_t,
                           const std::vector<RecipeManager::ItemStack>&) override {}

    void clear() { updates.clear(); }
};

// ---------------------------------------------------------------------------
// Recording FluidClient. FluidClient is a concrete class with virtual
// publishNodeUpdate / sendFluidRequest, so a subclass intercepts both before
// they can reach the (never connected) router — the same shape as
// test_machine_system.cpp's RecordingFluidClient. The base has NO virtual
// destructor, so it is handed to the system as a non-owning shared_ptr with a
// no-op deleter: deleting through a FluidClient* would be UB.
// ---------------------------------------------------------------------------

struct RecordingFluidClient : simcore::FluidClient {
    struct Call {
        uint64_t node_id = 0;
        int32_t x = 0, y = 0, z = 0;
        uint32_t fluid_id = 0;
        int32_t amount = 0;
        int32_t capacity = 0;
        int32_t max_input = 0;
        int32_t max_output = 0;
        int32_t tier = 0;
        bool is_source = false;
        bool is_sink = false;
    };
    std::vector<Call> node_updates;
    std::vector<Call> requests;

    RecordingFluidClient() : simcore::FluidClient(nullptr) {}

    void publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                           uint32_t fluid_id, int32_t amount, int32_t capacity,
                           int32_t max_input, int32_t max_output, int32_t tier,
                           bool is_source, bool is_sink,
                           const std::vector<uint64_t>&) override {
        node_updates.push_back(Call{node_id, x, y, z, fluid_id, amount, capacity,
                                    max_input, max_output, tier, is_source, is_sink});
    }

    void sendFluidRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                          uint32_t fluid_id, int32_t amount) override {
        requests.push_back(Call{node_id, x, y, z, fluid_id, amount, 0, 0, 0, 0,
                                false, false});
    }

    void clear() { node_updates.clear(); requests.clear(); }
};

// Process-wide singleton, so the non-owning pointer has a stable address.
static RecordingFluidClient& sharedFluidClient() {
    static RecordingFluidClient client;
    return client;
}

static std::shared_ptr<simcore::FluidClient> nonOwningFluidClient() {
    return std::shared_ptr<simcore::FluidClient>(&sharedFluidClient(),
                                                  [](simcore::FluidClient*) {});
}

static std::shared_ptr<simcore::IEventPublisher> recordingPublisher(RecordingPublisher** out) {
    auto p = std::make_shared<RecordingPublisher>();
    *out = p.get();
    return p;
}

// A machine entity with no FluidStorage — the real state of a creative fluid
// block, since machines.yaml declares no `fluid:` section for either block.
struct Fixture {
    entt::registry reg;
    RecordingPublisher* events_raw = nullptr;
    std::shared_ptr<simcore::IEventPublisher> events;
    std::unique_ptr<CreativeFluidSystem> sys;

    explicit Fixture(std::shared_ptr<simcore::FluidClient> fluid = nullptr) {
        events = recordingPublisher(&events_raw);
        sys = std::make_unique<CreativeFluidSystem>(reg, events, fluid);
    }

    // Plain machine, no fluid buffer: what SimulationEngine actually builds.
    entt::entity addMachine(uint16_t block_id, uint32_t x, uint32_t y, uint32_t z) {
        auto e = reg.create();
        reg.emplace<MachineComponent>(e, block_id, 0, x, y, z, 1);
        // machines.yaml gives these blocks an `energy:` block, so the engine
        // also creates an EnergyStorage. Present here to prove the system does
        // not depend on it.
        reg.emplace<EnergyStorage>(e, 1000000, 0, 0, 100000, 10,
                                   EnergyType::ELECTRICITY);
        return e;
    }

    void tick() { sys->tick(kDt); }
};

} // namespace

// ---------------------------------------------------------------------------
// 1. The block id -> fluid id mapping: two blocks, two DIFFERENT fluids.
// ---------------------------------------------------------------------------

static void test_CreativeFluidSystem_each_block_id_maps_to_its_own_fluid() {
    CHECK_EQ_I(CreativeFluidSystem::kOilFluidId, kOilFluid,
               "oil fluid id is 1111:11:58 (64570), the fluids.csv row");
    CHECK_EQ_I(CreativeFluidSystem::kWaterFluidId, kWaterFluid,
               "water fluid id is 1111:11:0 (64512), the fluids.csv row");

    CHECK_EQ_I(CreativeFluidSystem::fluidForBlock(kOilGenerator), kOilFluid,
               "creative_oil_generator produces oil");
    CHECK_EQ_I(CreativeFluidSystem::fluidForBlock(kWaterGenerator), kWaterFluid,
               "creative_water_generator produces water");
    CHECK_NE_I(CreativeFluidSystem::fluidForBlock(kOilGenerator),
               CreativeFluidSystem::fluidForBlock(kWaterGenerator),
               "the two blocks must not collapse to one fluid");

    CHECK_EQ_I(CreativeFluidSystem::fluidForBlock(kEnergyGenerator), 0,
               "the creative ENERGY generator is not a fluid source");
    CHECK_EQ_I(CreativeFluidSystem::fluidForBlock(kHeatFurnace), 0,
               "an unrelated machine is not a fluid source");
}

static void test_CreativeFluidSystem_oil_block_publishes_oil_as_a_source() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    auto e = f.addMachine(kOilGenerator, 10, 64, -5);

    f.tick();

    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(1),
               "the oil block publishes exactly one node update");
    if (sharedFluidClient().node_updates.empty()) return;
    const auto& call = sharedFluidClient().node_updates[0];

    CHECK_EQ_I(call.node_id, static_cast<uint64_t>(e),
               "the entity is the node id");
    CHECK_EQ_I(call.x, 10, "x forwarded");
    CHECK_EQ_I(call.y, 64, "y forwarded");
    CHECK_EQ_I(call.z, -5, "z forwarded — the uint32_t machine coord cast to int32_t");
    CHECK_EQ_I(call.fluid_id, kOilFluid, "the published fluid is OIL");
    CHECK_EQ_I(call.tier, CreativeFluidSystem::kDefaultTier,
               "tier comes from the creative generators' machines.yaml tier");
    CHECK(call.is_source, "is_source must be true so consumers can pull from it");
    CHECK(!call.is_sink, "is_sink must be false");

    auto* fluid = f.reg.try_get<FluidStorage>(e);
    CHECK(fluid != nullptr, "the system lazily creates the machine's fluid buffer");
    if (fluid) {
        CHECK_EQ_I(fluid->fluid_id, kOilFluid, "the buffer holds oil");
        CHECK_EQ_I(fluid->capacity, CreativeFluidSystem::kDefaultFluidCapacity,
                   "buffer capacity matches the creative generators' machines.yaml");
        CHECK_EQ_I(fluid->amount, CreativeFluidSystem::kDefaultFluidPerTick,
                   "one tick adds exactly one per-tick rate, not a full buffer");
        CHECK(fluid->amount < fluid->capacity, "one tick from empty does not fill a 1e6 buffer");
    }

    // The source is INFINITE: repeatedly topping up converges on capacity and
    // stops there, never exceeding it.
    for (int i = 0; i < 20; ++i) f.tick();
    CHECK_EQ_I(fluid->amount, fluid->capacity,
               "20 ticks of topping up fills the buffer exactly to capacity");
    CHECK(fluid->amount <= fluid->capacity, "and never overshoots it");
}

static void test_CreativeFluidSystem_water_block_publishes_water_not_oil() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    f.addMachine(kWaterGenerator, 1, 2, 3);

    f.tick();

    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(1), "one node update");
    if (sharedFluidClient().node_updates.empty()) return;
    CHECK_EQ_I(sharedFluidClient().node_updates[0].fluid_id, kWaterFluid,
               "the published fluid is WATER");
    CHECK_NE_I(sharedFluidClient().node_updates[0].fluid_id, kOilFluid,
               "the water block must never advertise oil");
    CHECK(sharedFluidClient().node_updates[0].is_source, "is_source true");
}

// ---------------------------------------------------------------------------
// 2. Non-creative machines are untouched.
// ---------------------------------------------------------------------------

static void test_CreativeFluidSystem_non_creative_machine_is_untouched() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    auto furnace = f.addMachine(kHeatFurnace, 5, 5, 5);
    auto energy_gen = f.addMachine(kEnergyGenerator, 6, 5, 5);

    f.tick();

    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(0),
               "a heat furnace and the creative energy generator publish no fluid node");
    CHECK(!f.reg.all_of<FluidStorage>(furnace),
           "no fluid buffer is created for a non-creative machine");
    CHECK(!f.reg.all_of<FluidStorage>(energy_gen),
           "no fluid buffer is created for the creative energy generator");
    CHECK_EQ_I(f.events_raw->updates.size(), size_t(0),
               "no block entity update is published for a non-creative machine");
}

static void test_CreativeFluidSystem_both_sources_publish_distinctly() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    f.addMachine(kOilGenerator, 0, 0, 0);
    f.addMachine(kWaterGenerator, 1, 0, 0);

    f.tick();

    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(2),
               "both creative sources publish in the same tick");
    if (sharedFluidClient().node_updates.size() != 2) return;
    CHECK_NE_I(sharedFluidClient().node_updates[0].fluid_id,
               sharedFluidClient().node_updates[1].fluid_id,
               "the two nodes carry different fluids, not one shared constant");
}

// ---------------------------------------------------------------------------
// 3. A full buffer does not overflow.
// ---------------------------------------------------------------------------

static void test_CreativeFluidSystem_full_buffer_does_not_overflow() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    auto e = f.addMachine(kWaterGenerator, 7, 7, 7);
    f.reg.emplace<FluidStorage>(e, kWaterFluid, /*amount=*/500, /*capacity=*/500,
                                /*maxIn=*/0, /*maxOut=*/100000);

    f.tick();
    f.tick();

    auto* fluid = f.reg.try_get<FluidStorage>(e);
    CHECK(fluid != nullptr, "precondition: buffer exists");
    if (!fluid) return;
    CHECK_EQ_I(fluid->amount, 500, "an already-full buffer is not pushed past capacity");
    CHECK(fluid->amount <= fluid->capacity, "amount never exceeds capacity");

    // A full source must STILL publish, or PipeNetwork drops the node and every
    // downstream consumer silently unhooks.
    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(2),
               "a full creative source keeps publishing every tick");
}

static void test_CreativeFluidSystem_partial_buffer_tops_up_without_overflow() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    auto e = f.addMachine(kOilGenerator, 8, 8, 8);
    f.reg.emplace<FluidStorage>(e, kOilFluid, /*amount=*/950, /*capacity=*/1000,
                                /*maxIn=*/0, /*maxOut=*/100000);
    f.sys->setFluidPerTick(500);  // more than the 50 mB of headroom

    f.tick();

    auto* fluid = f.reg.try_get<FluidStorage>(e);
    CHECK(fluid != nullptr, "precondition: buffer exists");
    if (!fluid) return;
    CHECK_EQ_I(fluid->amount, 1000, "clamped to the 50 mB of headroom, not 500");
    CHECK(fluid->amount <= fluid->capacity, "amount never exceeds capacity");
}

static void test_CreativeFluidSystem_zero_capacity_buffer_never_goes_negative() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    auto e = f.addMachine(kOilGenerator, 9, 9, 9);
    // Degenerate: capacity 0 is trivially "full" (amount >= capacity). The
    // refill runs BEFORE the isFull-style test precisely so this does not wedge
    // the source into permanent silence.
    f.reg.emplace<FluidStorage>(e, kOilFluid, 0, 0, 0, 100000);

    f.tick();

    auto* fluid = f.reg.try_get<FluidStorage>(e);
    CHECK(fluid != nullptr, "precondition: buffer exists");
    if (!fluid) return;
    CHECK_EQ_I(fluid->amount, 0, "a zero-capacity buffer stays at zero, never negative");
    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(1),
               "a zero-capacity source still registers its node with the network");
}

// ---------------------------------------------------------------------------
// 4. Safety: wrong-fluid buffers, missing clients, no EnergyStorage.
// ---------------------------------------------------------------------------

static void test_CreativeFluidSystem_a_foreign_fluid_buffer_is_not_overwritten() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    auto e = f.addMachine(kOilGenerator, 11, 11, 11);
    // Something else legitimately produced this fluid into the machine.
    f.reg.emplace<FluidStorage>(e, kWaterFluid, 42, 100, 0, 100);

    f.tick();

    auto* fluid = f.reg.try_get<FluidStorage>(e);
    CHECK(fluid != nullptr, "precondition: buffer exists");
    if (!fluid) return;
    CHECK_EQ_I(fluid->fluid_id, kWaterFluid, "the existing fluid is left alone");
    CHECK_EQ_I(fluid->amount, 42, "the amount is left alone");
    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(0),
               "a mismatched buffer does not advertise a lie to the network");
}

static void test_CreativeFluidSystem_null_fluid_client_is_safe() {
    Fixture f(nullptr);  // no fluid client at all
    auto e = f.addMachine(kOilGenerator, 12, 12, 12);

    f.tick();

    auto* fluid = f.reg.try_get<FluidStorage>(e);
    CHECK(fluid != nullptr, "the buffer is still filled with no client present");
    if (fluid) {
        CHECK_EQ_I(fluid->fluid_id, kOilFluid, "correct fluid with no client");
        CHECK(fluid->amount > 0, "buffer was still filled with no client");
    }
    CHECK_EQ_I(f.events_raw->updates.size(), size_t(1),
               "the block entity update still goes out");
}

static void test_CreativeFluidSystem_works_without_energy_storage() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    // A machine with NO EnergyStorage at all. The view must not require it.
    auto e = f.reg.create();
    f.reg.emplace<MachineComponent>(e, kWaterGenerator, 0, 1, 2, 3, 1);

    f.tick();

    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(1),
               "a fluid source with no EnergyStorage still publishes");
    if (!sharedFluidClient().node_updates.empty()) {
        CHECK_EQ_I(sharedFluidClient().node_updates[0].fluid_id, kWaterFluid,
                   "and publishes the right fluid");
    }
    CHECK(f.reg.try_get<FluidStorage>(e) != nullptr, "buffer created");
}

static void test_CreativeFluidSystem_empty_registry_is_a_noop() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());

    f.tick();

    CHECK_EQ_I(sharedFluidClient().node_updates.size(), size_t(0), "nothing published");
    CHECK_EQ_I(f.events_raw->updates.size(), size_t(0), "no events published");
}

static void test_CreativeFluidSystem_never_publishes_a_fluid_request() {
    sharedFluidClient().clear();
    Fixture f(nonOwningFluidClient());
    f.addMachine(kOilGenerator, 13, 13, 13);
    f.addMachine(kWaterGenerator, 14, 13, 13);

    f.tick();

    // A source pushes, it never pulls. A sendFluidRequest here would make the
    // generator a sink of its own fluid and deadlock the network solve.
    CHECK_EQ_I(sharedFluidClient().requests.size(), size_t(0),
               "a creative source never requests fluid");
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while (0)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printf("=== creative_fluid_system test suite ===\n\n");

    TEST(CreativeFluidSystem_each_block_id_maps_to_its_own_fluid);
    TEST(CreativeFluidSystem_oil_block_publishes_oil_as_a_source);
    TEST(CreativeFluidSystem_water_block_publishes_water_not_oil);
    TEST(CreativeFluidSystem_non_creative_machine_is_untouched);
    TEST(CreativeFluidSystem_both_sources_publish_distinctly);
    TEST(CreativeFluidSystem_full_buffer_does_not_overflow);
    TEST(CreativeFluidSystem_partial_buffer_tops_up_without_overflow);
    TEST(CreativeFluidSystem_zero_capacity_buffer_never_goes_negative);
    TEST(CreativeFluidSystem_a_foreign_fluid_buffer_is_not_overwritten);
    TEST(CreativeFluidSystem_null_fluid_client_is_safe);
    TEST(CreativeFluidSystem_works_without_energy_storage);
    TEST(CreativeFluidSystem_empty_registry_is_a_noop);
    TEST(CreativeFluidSystem_never_publishes_a_fluid_request);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
