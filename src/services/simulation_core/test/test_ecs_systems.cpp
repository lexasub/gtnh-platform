#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <unistd.h>

#include <entt/entt.hpp>

#include "Network/clients/IoUringRouterClient.h"
#include "ECS/SimulationEngine.h"
#include "ECS/PatternLibrary.h"
#include "multiblock_state_generated.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/RecipeProgress.h"
#include "ECS/components/InventoryContainer.h"
#include "ECS/components/EnergyStorage.h"
#include "ECS/components/Position.h"
#include "ECS/Systems/GeneratorSystem.h"
#include "ECS/Systems/AdjacencyTransferSystem.h"
#include "ECS/Systems/MachineSystem.h"
#include "ECS/Systems/CreativeGeneratorSystem.h"
#include "ECS/Systems/BoilerSystem.h"
#include "ECS/Systems/BatteryBufferSystem.h"
#include "ECS/Systems/DrillSystem.h"
#include "ECS/components/DrillComponent.h"
#include "Network/IEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "MachineRegistry.h"
#include "RecipeManager/RecipeManager.h"
#include "Actions/SetBlockCASHandler.h"
#include "Storage/IBlockRepository.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <common/ItemId.h>

extern int g_tests, g_passed, g_failed;
void test_check(bool cond, const char* file, int line, const char* expr, const char* msg);

#ifndef CHECK_EQ
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#endif
#ifndef CHECK_NE
#define CHECK_NE(a, b, msg) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, msg)
#endif
#ifndef CHECK_GT
#define CHECK_GT(a, b, msg) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, msg)
#endif
#ifndef CHECK_LT
#define CHECK_LT(a, b, msg) test_check((a) < (b), __FILE__, __LINE__, #a " < " #b, msg)
#endif
#ifndef CHECK
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#endif
#ifndef PASS
#define PASS() do { ++g_passed; } while(0)
#endif

static std::string makeTempFile(const std::string& content) {
    char tmpl[] = "/tmp/machine_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return {};
    [[maybe_unused]] ssize_t wr = write(fd, content.data(), content.size());
    close(fd);
    return std::string(tmpl);
}

struct MockEventPublisher : simcore::IEventPublisher {
    int block_ack_count = 0;
    int block_changed_count = 0;
    int block_entity_update_count = 0;
    int block_directive_count = 0;
    int last_x = 0, last_y = 0, last_z = 0;
    uint16_t last_machine_id = 0;
    float last_progress = 0;
    uint32_t last_energy = 0;

    void publishBlockAck(uint8_t, int32_t x, int32_t y, int32_t z,
                         uint16_t, uint8_t, const char*, uint32_t,
                         uint8_t = 1) override {
        block_ack_count++;
        last_x = x; last_y = y; last_z = z;
    }
    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t, uint8_t) override {
        block_directive_count++;
    }
    void publishBlockChangedEvent(int32_t, int32_t, int32_t,
                                  uint16_t, uint8_t, uint32_t, uint64_t) override {
        block_changed_count++;
    }
    void publishBlockEntityUpdate(int32_t x, int32_t y, int32_t z,
                                   uint16_t machine_type,
                                   const std::vector<uint8_t>&,
                                   float progress,
                                   uint32_t energy,
                                   EnergyType,
                                   uint32_t,
                                   int,
                                   float,
                                   const std::vector<HatchUpdateData>* = nullptr) override {
        block_entity_update_count++;
        last_x = x; last_y = y; last_z = z;
        last_machine_id = machine_type;
        last_progress = progress;
        last_energy = energy;
    }

    void publishMachineSlotResponse(int32_t, int32_t, int32_t,
                                    uint16_t, bool,
                                    uint16_t, uint8_t, uint16_t,
                                    const char*) override {}

    void publishMachineConfigUpdatedEvent(int32_t, int32_t, int32_t,
                                          const std::array<uint8_t, 6>&) override {}

    void publishMultiblockCreated(uint64_t, int32_t, int32_t, int32_t,
                                   uint16_t) override {}
    void publishMultiblockDestroyed(uint64_t) override {}
    void publishGridUpdate(int32_t, int32_t, int32_t,
                          const std::vector<RecipeManager::ItemStack>&) override {}
};

struct MockBlockRepository : simcore::IBlockRepository {
    int set_cas_calls = 0;
    void setBlockCAS(int32_t, int32_t, int32_t, uint16_t, uint16_t, uint8_t,
                     simcore::IBlockRepository::SetBlockCASCallback) override {
        set_cas_calls++;
    }
    void getBlock(int32_t, int32_t, int32_t,
                  simcore::IBlockRepository::GetBlockCallback cb) override {
        cb({0, 0, 0}); // not an ore block -> drill stays SEARCHING
    }
};

// ChunkStore-backed repo whose getBlock returns a pre-existing block — the
// scenario of a machine block placed before this simcore instance started
// (the block persists in the world, but no ECS entity was ever created).
struct FakeBlockRepository : simcore::IBlockRepository {
    uint16_t block_id = 0;
    uint8_t meta = 0;
    uint32_t mb_id = 0;
    int get_calls = 0;
    int set_cas_calls = 0;
    void setBlockCAS(int32_t, int32_t, int32_t, uint16_t, uint16_t, uint8_t,
                     simcore::IBlockRepository::SetBlockCASCallback cb) override {
        set_cas_calls++;
        cb({0, 0, 0});
    }
    void getBlock(int32_t, int32_t, int32_t,
                  simcore::IBlockRepository::GetBlockCallback cb) override {
        get_calls++;
        cb({block_id, meta, mb_id});
    }
};

static std::string g_consumersPath, g_producersPath;

void setupMachineRegistry() {
    std::string consumers =
        "id,name,class,energy_in,tier,slots_in,slots_out,capacity,maxInput,maxOutput\n"
        "36,gtnh:heat_furnace,Furnace,HEAT,0,1,1,10000,32,0\n";
    std::string producers =
        "id,name,class,energy_out,energy_in,tier,slots_in,slots_out,capacity,maxInput,maxOutput\n"
        "46,gtnh:heat_generator,Generator,HEAT,,0,1,0,0,32,32\n"
        "63,gtnh:creative_generator,CreativeGenerator,ELECTRICITY,,10,0,0,1000000,100000,0\n";
    g_consumersPath = makeTempFile(consumers);
    g_producersPath = makeTempFile(producers);

    auto reg = MachineRegistry::Load(g_consumersPath.c_str(), g_producersPath.c_str());
    MachineRegistry::setInstance(reg.get());
    reg.release();
}

// machines.yaml uses lowercase role ("producer"/"consumer"). ParseRole must be
// case-insensitive, otherwise every producer is parsed as CONSUMER →
// maxOutput=0 (generators never produce) and maxInput=usage-default 32.
static void test_MachineRegistry_Yaml_lowercase_role() {
    std::string yaml =
        "machine_classes:\n"
        "  - class: generator\n"
        "    variants:\n"
        "      - block_id: \"1110:00:2\"\n"
        "        name: heat_generator\n"
        "        energy_out: HEAT\n"
        "        role: producer\n"
        "        slots: { input: 1, output: 0 }\n"
        "        energy:\n"
        "          capacity: 10000\n"
        "          max_output: 32\n"
        "  - class: macerator\n"
        "    variants:\n"
        "      - block_id: \"1110:00:8\"\n"
        "        name: macerator\n"
        "        energy_in: ELECTRICITY\n"
        "        role: consumer\n"
        "        slots: { input: 1, output: 1 }\n"
        "        energy:\n"
        "          capacity: 5000\n"
        "          usage: 16\n";
    std::string path = makeTempFile(yaml);
    auto reg = MachineRegistry::LoadFromYaml(path.c_str());
    CHECK(reg != nullptr, "YAML registry should load");

    auto* gen = reg->Get(ItemId::pack("1110:00:2"));
    CHECK(gen != nullptr, "heat_generator should load from YAML");
    if (gen) {
        CHECK_EQ(static_cast<int>(gen->role), static_cast<int>(MachineRole::PRODUCER),
                 "lowercase 'producer' must parse as PRODUCER");
        CHECK_EQ(gen->maxOutput, 32, "producer must read max_output");
        CHECK_EQ(gen->maxInput, 0, "producer without usage must have maxInput 0");
        CHECK(gen->energy_out.has_value(), "energy_out must parse");
        if (gen->energy_out.has_value()) {
            CHECK_EQ(static_cast<int>(gen->energy_out.value()),
                     static_cast<int>(EnergyType::HEAT), "energy_out must be HEAT");
        }
    }

    auto* mac = reg->Get(ItemId::pack("1110:00:8"));
    CHECK(mac != nullptr, "macerator should load from YAML");
    if (mac) {
        CHECK_EQ(static_cast<int>(mac->role), static_cast<int>(MachineRole::CONSUMER),
                 "lowercase 'consumer' must parse as CONSUMER");
        CHECK_EQ(mac->maxInput, 16, "consumer must read usage");
        CHECK(mac->energy_in.has_value(), "energy_in must parse");
    }
    PASS();
}

static void test_GeneratorSystem_burns_coal() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem sys(reg, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, ItemId::pack("1110:00:2"), 0, 100, 64, 100, 1);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 0, 128, 128, 0, EnergyType::HEAT);
    simcore::InventoryContainer container(0, 1, {{ItemId::pack("0:11110:2"), 1, 0}});
    reg.emplace<simcore::InventoryContainer>(ent, container);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_GT(energy.current, 0, "generator should produce energy from coal");
    CHECK_GT(events->block_entity_update_count, 0, "should publish BlockEntityUpdate");
    CHECK_EQ(events->last_machine_id, ItemId::pack("1110:00:2"), "machine_id should be heat_generator");

    PASS();
}

static void test_GeneratorSystem_producer_maxInput_zero() {
    // Mirrors the runtime producer config from machines.yaml: the generator's
    // EnergyStorage has maxInput=0 (producers have no external input). addEnergy
    // clamps by maxInput, so production must use produceEnergy instead.
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem sys(reg, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, ItemId::pack("1110:00:2"), 0, 200, 64, 200, 4);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 0, 0, 32, 0, EnergyType::HEAT);
    simcore::InventoryContainer container(0, 1, {{ItemId::pack("0:11110:2"), 1, 0}});
    reg.emplace<simcore::InventoryContainer>(ent, container);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_GT(energy.current, 0, "generator produces energy even with maxInput=0");

    PASS();
}

static void test_GeneratorSystem_no_fuel_no_energy() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem sys(reg, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, ItemId::pack("1110:00:2"), 0, 101, 64, 101, 2);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 0, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::InventoryContainer>(ent);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_EQ(energy.current, 0, "no fuel means no energy");

    PASS();
}

static void test_GeneratorSystem_full_storage_skips() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem sys(reg, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, ItemId::pack("1110:00:2"), 0, 102, 64, 102, 3);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 10000, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::InventoryContainer>(ent);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_EQ(energy.current, 10000, "full storage should stay full");

    PASS();
}

static void test_AdjacencyTransferSystem_adjacent_transfer() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem genSys(reg, events, pipeClient);
    simcore::AdjacencyTransferSystem adjSys(reg, *MachineRegistry::instance(), events);

    auto gen = reg.create();
    reg.emplace<simcore::MachineComponent>(gen, 46, 0, 0, 0, 0, 10);
    reg.emplace<simcore::EnergyStorage>(gen, 10000, 5000, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::Position>(gen, 0, 0, 0);
    reg.emplace<simcore::InventoryContainer>(gen);
    {
        auto& inv = reg.get<simcore::InventoryContainer>(gen);
        inv.slots.push_back({44, 64, 0});
    }

    auto furn = reg.create();
    reg.emplace<simcore::MachineComponent>(furn, 36, 0, 1, 0, 0, 11);
    reg.emplace<simcore::EnergyStorage>(furn, 10000, 0, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::Position>(furn, 1, 0, 0);

    genSys.tick(0.05f);
    adjSys.tick(0.05f);

    auto& genEnergy = reg.get<simcore::EnergyStorage>(gen);
    auto& furnEnergy = reg.get<simcore::EnergyStorage>(furn);

    CHECK_GT(furnEnergy.current, 0, "adjacent furnace should receive heat");
    CHECK_LT(genEnergy.current, 5000, "generator energy decreased after transfer");

    PASS();
}

static void test_AdjacencyTransferSystem_non_adjacent_no_transfer() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem genSys(reg, events, pipeClient);
    simcore::AdjacencyTransferSystem adjSys(reg, *MachineRegistry::instance(), events);

    auto gen = reg.create();
    reg.emplace<simcore::MachineComponent>(gen, 46, 0, 0, 0, 0, 20);
    reg.emplace<simcore::EnergyStorage>(gen, 10000, 5000, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::Position>(gen, 0, 0, 0);
    reg.emplace<simcore::InventoryContainer>(gen);

    auto furn = reg.create();
    reg.emplace<simcore::MachineComponent>(furn, 36, 0, 10, 0, 0, 21);
    reg.emplace<simcore::EnergyStorage>(furn, 10000, 0, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::Position>(furn, 10, 0, 0);

    adjSys.tick(0.05f);

    auto& furnEnergy = reg.get<simcore::EnergyStorage>(furn);
    CHECK_EQ(furnEnergy.current, 0, "non-adjacent furnace gets no heat");

    PASS();
}

// Heat propagation end-to-end through the real onBlockChanged entity-creation
// path with the real machines.yaml: heat_generator (1110:00:2, PRODUCER/HEAT)
// burning coal must transfer heat into an adjacent heat_furnace (1110:00:0,
// CONSUMER/HEAT). Guards the ParseRole + HeatTransferSystem producer-detection
// fixes — before them the generator parsed as CONSUMER (maxOutput=0) and was
// never treated as a heat source.
static void test_HeatTransferSystem_yaml_generator_to_furnace() {
    auto reg = MachineRegistry::LoadFromYaml(DATA_DIR "/registry/machines.yaml");
    CHECK(reg != nullptr, "real machines.yaml should load");
    if (!reg) { PASS(); return; }
    MachineRegistry::setInstance(reg.get());

    simcore::SimulationEngine engine;
    engine.setMachineRegistry(reg.get());
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem genSys(engine.reg(), events, pipeClient);
    simcore::AdjacencyTransferSystem adjSys(engine.reg(), *MachineRegistry::instance(), events);

    engine.onBlockChanged(0, 0, 0, ItemId::pack("1110:00:2"), 0, 0); // heat_generator
    engine.onBlockChanged(1, 0, 0, ItemId::pack("1110:00:0"), 0, 0); // heat_furnace

    // Verify the registry-derived components are correct (root-cause guards).
    entt::entity gen = entt::null, furn = entt::null;
    auto& r = engine.reg();
    for (auto e : r.view<simcore::Position>()) {
        auto& pos = r.get<simcore::Position>(e);
        auto* es = r.try_get<simcore::EnergyStorage>(e);
        CHECK(es != nullptr, "machine entity has EnergyStorage");
        if (pos.x == 0 && es) {
            gen = e;
            CHECK_EQ(static_cast<int>(es->type), static_cast<int>(EnergyType::HEAT),
                     "heat_generator must be HEAT type");
            CHECK_EQ(es->maxOutput, 32, "heat_generator must have maxOutput from yaml");
            CHECK_EQ(es->maxInput, 0, "heat_generator (producer) must have maxInput 0");
        } else if (pos.x == 1 && es) {
            furn = e;
            CHECK_EQ(static_cast<int>(es->type), static_cast<int>(EnergyType::HEAT),
                     "heat_furnace must be HEAT type");
        }
    }
    CHECK(gen != entt::null, "heat_generator entity created");
    CHECK(furn != entt::null, "heat_furnace entity created");

    // Put coal in the generator's slot 0 (same as MachineSlotHandler would).
    if (auto* c = r.try_get<simcore::InventoryContainer>(gen)) {
        if (c->slots.size() > 0) {
            c->slots[0] = {ItemId::pack("0:11110:2"), 64, 0};
        }
    }

    for (int i = 0; i < 20; ++i) {
        genSys.tick(0.05f);
        adjSys.tick(0.05f);
    }

    auto& fe = r.get<simcore::EnergyStorage>(furn);
    auto& ge = r.get<simcore::EnergyStorage>(gen);
    CHECK_GT(fe.current, 0, "adjacent heat_furnace receives heat from heat_generator");
    CHECK_LT(ge.current, 10000, "generator heat transferred away (not full)");
    CHECK_GT(fe.current, 100, "furnace accumulated meaningful heat over 20 ticks");

    PASS();
}

// A machine block that predates this simcore instance (exists only in the
// block repository, no ECS entity) must be lazy-created on right-click —
// otherwise it is invisible to GeneratorSystem/MachineSystem/HeatTransferSystem
// and heat can never reach it. Guards the SetBlockCASHandler lazy-init fix.
static void test_SetBlockCASHandler_lazy_creates_pre_existing_machine() {
    auto reg = MachineRegistry::LoadFromYaml(DATA_DIR "/registry/machines.yaml");
    CHECK(reg != nullptr, "real machines.yaml should load");
    if (!reg) { PASS(); return; }
    MachineRegistry::setInstance(reg.get());

    auto engine = std::make_shared<simcore::SimulationEngine>();
    engine->setMachineRegistry(reg.get());
    auto events = std::make_shared<MockEventPublisher>();

    // The furnace "exists" in the world (ChunkStore) but has no ECS entity —
    // the scenario of a block placed before simcore restarted.
    auto repo = std::make_shared<FakeBlockRepository>();
    repo->block_id = ItemId::pack("1110:00:0"); // heat_furnace
    repo->meta = 0;
    repo->mb_id = 0;

    simcore::SetBlockCASHandler handler(repo, events, engine);

    // Client right-clicks the furnace at (5,10,5) to open its window.
    flatbuffers::FlatBufferBuilder fb(256);
    Protocol::Vec3i pos(5, 10, 5);
    auto action = Protocol::CreateSetBlockAction(
        fb, /*player_id=*/1, Protocol::PlayerActionType_RIGHT_MOUSE_CLICK,
        &pos, /*expected_block_id=*/repo->block_id, /*new_block_id=*/0,
        /*request_id=*/7);
    fb.Finish(action);
    // Mirrors SimCoreMessageHandler: GetRoot + cast to void* to hit the
    // public IActionHandler::handle(const void*) overload.
    handler.handle(static_cast<void*>(
        const_cast<Protocol::SetBlockAction*>(
            flatbuffers::GetRoot<Protocol::SetBlockAction>(fb.GetBufferPointer()))));

    // Right-click must have queried ChunkStore, created the entity, and
    // published its state.
    CHECK_EQ(repo->get_calls, 1, "right-click queried ChunkStore for the block");
    CHECK_EQ(events->block_entity_update_count, 1, "right-click published machine state");
    CHECK_EQ(events->last_machine_id, repo->block_id, "published state is for the furnace");

    entt::entity furn = entt::null;
    auto& r = engine->reg();
    for (auto e : r.view<simcore::Position>()) {
        auto& pp = r.get<simcore::Position>(e);
        if (pp.x == 5 && pp.y == 10 && pp.z == 5) furn = e;
    }
    CHECK(furn != entt::null, "furnace entity lazy-created from ChunkStore");
    if (furn == entt::null) { PASS(); return; }

    // Place a heat_generator adjacent and burn coal — heat must now flow into
    // the lazy-created furnace (the pre-fix behaviour: no entity → no transfer).
    engine->onBlockChanged(4, 10, 5, ItemId::pack("1110:00:2"), 0, 0);
    entt::entity gen = entt::null;
    for (auto e : r.view<simcore::Position>()) {
        auto& pp = r.get<simcore::Position>(e);
        if (pp.x == 4 && pp.y == 10 && pp.z == 5) gen = e;
    }
    CHECK(gen != entt::null, "generator entity created");
    if (gen == entt::null) { PASS(); return; }
    if (auto* c = r.try_get<simcore::InventoryContainer>(gen)) {
        if (!c->slots.empty()) c->slots[0] = {ItemId::pack("0:11110:2"), 64, 0};
    }

    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(
        std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem genSys(r, events, pipeClient);
    simcore::AdjacencyTransferSystem adjSys(r, *MachineRegistry::instance(), events);
    for (int i = 0; i < 20; ++i) {
        genSys.tick(0.05f);
        adjSys.tick(0.05f);
    }

    auto& fe = r.get<simcore::EnergyStorage>(furn);
    CHECK_GT(fe.current, 0, "lazy-created furnace receives heat from generator");

    PASS();
}

static void test_MachineSystem_idle_no_recipe() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    auto recipes = std::make_shared<RecipeManager::RecipeManager>();
    simcore::MachineSystem sys(reg, recipes, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 36, 0, 200, 50, 200, 30);
    reg.emplace<simcore::RecipeProgress>(ent);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 1000, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::InventoryContainer>(ent);

    sys.tick(0.05f);
    auto& progress = reg.get<simcore::RecipeProgress>(ent);

    CHECK(progress.recipe_id.empty(), "no input -> no recipe");
    CHECK(!progress.is_processing, "not processing without recipe");

    PASS();
}

static void test_BatteryBufferSystem_charges_tool() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();

    auto ent = reg.create();
    reg.emplace<simcore::Position>(ent, 0, 0, 0);
    reg.emplace<simcore::BatteryBufferComponent>(ent,
        40000,   // capacity
        20000,   // stored EU
        1,       // tier LV
        32,      // maxInput
        8,       // chargeRate
        1        // numSlots
    );
    reg.emplace<simcore::InventoryContainer>(ent, 0, 1,
        std::vector<simcore::InventorySlot>{{90, 1, 0}}  // drill_ulv, meta=0 = 0 EU
    );

    simcore::BatteryBufferSystem sys(reg);
    sys.tick(0.05f);

    const auto& inv = reg.get<simcore::InventoryContainer>(ent);
    const auto& buf = reg.get<simcore::BatteryBufferComponent>(ent);

    // Tool should have gained some energy (max 8 per tick)
    CHECK_GT(inv.slots[0].meta, 0, "tool gained energy from buffer");
    CHECK_LT(inv.slots[0].meta, 9, "tool charged at most chargeRate per tick");
    // Battery should have lost that much
    CHECK_LT(buf.stored, 20000, "battery lost energy charging tool");

    PASS();
}

static void test_BatteryBufferSystem_empty_slot_noop() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();

    auto ent = reg.create();
    reg.emplace<simcore::Position>(ent, 0, 0, 0);
    reg.emplace<simcore::BatteryBufferComponent>(ent,
        40000, 20000, 1, 32, 8, 1
    );
    reg.emplace<simcore::InventoryContainer>(ent, 0, 1,
        std::vector<simcore::InventorySlot>{{0, 0, 0}}  // empty slot
    );

    simcore::BatteryBufferSystem sys(reg);
    sys.tick(0.05f);

    const auto& buf = reg.get<simcore::BatteryBufferComponent>(ent);
    CHECK_EQ(buf.stored, 20000, "no energy consumed for empty slot");

    PASS();
}

static void test_BatteryBufferSystem_full_tool_skips() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();

    auto ent = reg.create();
    reg.emplace<simcore::Position>(ent, 0, 0, 0);
    reg.emplace<simcore::BatteryBufferComponent>(ent,
        40000, 20000, 1, 32, 8, 1
    );
    // meta=1000 is drill_ulv full capacity
    reg.emplace<simcore::InventoryContainer>(ent, 0, 1,
        std::vector<simcore::InventorySlot>{{90, 1, 1000}}
    );

    simcore::BatteryBufferSystem sys(reg);
    sys.tick(0.05f);

    const auto& buf = reg.get<simcore::BatteryBufferComponent>(ent);
    CHECK_EQ(buf.stored, 20000, "no energy consumed for full tool");

    PASS();
}

static void test_CreativeGeneratorSystem_fills_energy() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::CreativeGeneratorSystem sys(reg, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, ItemId::pack("1110:01:2"), 0, 300, 50, 300, 40);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 0, 0, 0, 10, EnergyType::ELECTRICITY);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_GT(energy.current, 0, "creative generator fills energy");
    CHECK_GT(events->block_entity_update_count, 0, "publishes update");

    PASS();
}

static void test_MultiblockFormation_hatchIO() {
    setupMachineRegistry();
    auto* mreg = MachineRegistry::instance();

    // Register the EBF controller as a machine so the formation path triggers
    // (mirrors the runtime registration in simcored main.cpp).
    MachineInfo ebf{};
    ebf.id = 1003;
    ebf.name = "electric_blast_furnace";
    ebf.machine_class = "ebf";
    ebf.role = MachineRole::CONSUMER;
    ebf.energy_in = EnergyType::HEAT;
    ebf.tier = 1;
    ebf.slots_in = 0;
    ebf.slots_out = 0;
    ebf.capacity = 10000;
    ebf.maxInput = 32;
    ebf.maxOutput = 32;
    mreg->Register(ebf);

    auto engine = std::make_shared<simcore::SimulationEngine>();
    engine->setMachineRegistry(mreg);
    auto& reg = engine->reg();

    constexpr uint16_t CASING = 1001;
    constexpr uint16_t COIL = 1002;
    constexpr uint16_t CTRL = 1003;

    auto place = [&](int x, int y, int z, uint16_t id) {
        engine->onBlockChanged(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                               static_cast<uint32_t>(z), id, 0, 0);
    };

    // EBF footprint: controller at (0,3,0), corner at (-1,0,-1).
    // Layer 0 (y=0): full casing floor.
    for (int x = -1; x <= 1; ++x)
        for (int z = -1; z <= 1; ++z) place(x, 0, z, CASING);

    // Layer 1 (y=1): casing corners, coil center, ITEM_IN/OUT hatch side walls.
    place(-1, 1, -1, CASING); place(1, 1, -1, CASING);
    place(-1, 1, 1, CASING);  place(1, 1, 1, CASING);
    place(-1, 1, 0, simcore::HATCH_BLOCK_ITEM_IN);   // corner-rel (0,1,1)
    place(1, 1, 0, simcore::HATCH_BLOCK_ITEM_OUT);   // corner-rel (2,1,1)
    place(0, 1, 0, COIL);

    // Layer 2 (y=2): same shell.
    place(-1, 2, -1, CASING); place(1, 2, -1, CASING);
    place(-1, 2, 1, CASING);  place(1, 2, 1, CASING);
    place(0, 2, 0, COIL);

    // Layer 3 (y=3): full casing ring, then controller placed LAST so the
    // whole pattern is present when the formation check runs.
    for (int x = -1; x <= 1; ++x)
        for (int z = -1; z <= 1; ++z)
            if (!(x == 0 && z == 0)) place(x, 3, z, CASING);
    place(0, 3, 0, CTRL);

    auto& controllers = engine->getControllers();
    CHECK_EQ(static_cast<int>(controllers.size()), 1, "EBF controller formed");
    if (controllers.empty()) {
        PASS();
        return;
    }

    const auto& ctrl = controllers.begin()->second;
    CHECK_EQ(ctrl.pattern_id, 1u, "EBF pattern id");
    CHECK(ctrl.hatches.size() >= 4, "ITEM_IN/OUT + MUFFLER + ENERGY hatches detected");

    // ITEM_IN/OUT hatches carry the first 8 slots (4+4) after reorder.
    int in_start = -1, in_end = -1, out_start = -1, out_end = -1;
    simcore::SimulationEngine::getInputSlotRange(ctrl, in_start, in_end);
    simcore::SimulationEngine::getOutputSlotRange(ctrl, out_start, out_end);
    CHECK_EQ(in_start, 0, "ITEM_IN slots start at 0");
    CHECK_EQ(in_end, 4, "ITEM_IN hatch has 4 slots");
    CHECK_EQ(out_start, 4, "ITEM_OUT slots follow ITEM_IN");
    CHECK_EQ(out_end, 8, "ITEM_OUT hatch has 4 slots");

    // Controller entity container resized to hatch slots.
    entt::entity ctrl_entity = entt::null;
    auto view = reg.view<const simcore::Position, simcore::MachineComponent>();
    for (auto e : view) {
        auto& pos = view.get<const simcore::Position>(e);
        if (pos.x == 0 && pos.y == 3 && pos.z == 0) { ctrl_entity = e; break; }
    }
    CHECK(ctrl_entity != entt::null, "controller entity exists");
    if (ctrl_entity != entt::null) {
        auto& container = reg.get<simcore::InventoryContainer>(ctrl_entity);
        CHECK_EQ(static_cast<int>(container.slots.size()), 8, "8 hatch slots allocated");
        // Put an ore in the ITEM_IN hatch, verify the guard collects it.
        container.slots[0] = {3, 1, 0};
        std::vector<simcore::InventorySlot> contents;
        engine->collectControllerContents(ctrl, contents);
        CHECK_EQ(static_cast<int>(contents.size()), 1, "collectControllerContents gathers hatch items");
        CHECK_EQ(contents[0].item_id, 3u, "gathered item is the placed ore");

        // Persistence (task 2.2): serialized state carries the inventory.
        auto blob = engine->serializeMultiblock(ctrl.id);
        CHECK(!blob.empty(), "serializeMultiblock produces a blob");
        auto fb = flatbuffers::GetRoot<Protocol::MultiblockState>(blob.data());
        CHECK(fb->slots() != nullptr, "MultiblockState includes inventory slots");
        if (fb->slots()) {
            CHECK_EQ(static_cast<int>(fb->slots()->size()), 8, "8 slots persisted");
            CHECK_EQ(fb->slots()->Get(0)->item_id(), 3u, "ore restored from slot 0");
        }
    }

    PASS();
}

// ---------------------------------------------------------------------------
// DrillSystem — item-energy drill (issue: Wire DrillSystem item energy check)
// ---------------------------------------------------------------------------

static void test_DrillSystem_drains_tool_energy() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto blockRepo = std::make_shared<MockBlockRepository>();
    simcore::DrillSystem sys(reg, blockRepo, events, nullptr);

    auto ent = reg.create();
    simcore::DrillComponent drill(0, 0, 0, 0); // tier 0 -> energyPerTick = 10
    drill.state = simcore::DrillState::MINING;
    drill.targetX = 1; drill.targetY = 0; drill.targetZ = 0;
    drill.miningTicksTotal = 1;
    drill.miningProgress = 1;
    reg.emplace<simcore::DrillComponent>(ent, drill);
    // Full drill_ulv (item 90, capacity 1000 EU) in the machine inventory.
    reg.emplace<simcore::InventoryContainer>(ent, 0, 1,
        std::vector<simcore::InventorySlot>{{90, 1, 1000}});

    sys.tick(0.05f);

    const auto& inv = reg.get<simcore::InventoryContainer>(ent);
    const auto& d = reg.get<simcore::DrillComponent>(ent);
    CHECK_EQ(inv.slots[0].meta, uint16_t(990), "drill tool drained 10 EU per tick while MINING");
    CHECK_EQ(d.state, simcore::DrillState::MINING, "drill keeps mining while tool has energy");
    CHECK_EQ(blockRepo->set_cas_calls, 1, "mining progressed to block removal");
    PASS();
}

static void test_DrillSystem_insufficient_tool_energy_aborts() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto blockRepo = std::make_shared<MockBlockRepository>();
    simcore::DrillSystem sys(reg, blockRepo, events, nullptr);

    auto ent = reg.create();
    simcore::DrillComponent drill(0, 0, 0, 0); // energyPerTick = 10
    drill.state = simcore::DrillState::MINING;
    drill.targetX = 1; drill.targetY = 0; drill.targetZ = 0;
    drill.miningTicksTotal = 1;
    drill.miningProgress = 1;
    reg.emplace<simcore::DrillComponent>(ent, drill);
    // Only 5 EU left in the tool — less than the 10 EU/tick required.
    reg.emplace<simcore::InventoryContainer>(ent, 0, 1,
        std::vector<simcore::InventorySlot>{{90, 1, 5}});

    sys.tick(0.05f);

    const auto& inv = reg.get<simcore::InventoryContainer>(ent);
    const auto& d = reg.get<simcore::DrillComponent>(ent);
    CHECK_NE(d.state, simcore::DrillState::MINING, "insufficient tool energy aborts mining");
    CHECK_EQ(blockRepo->set_cas_calls, 0, "no block removal when energy insufficient");
    CHECK_EQ(inv.slots[0].meta, uint16_t(5), "tool energy unchanged when drain fails");
    PASS();
}

static void test_DrillSystem_falls_back_to_machine_energy() {
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto blockRepo = std::make_shared<MockBlockRepository>();
    simcore::DrillSystem sys(reg, blockRepo, events, nullptr);

    auto ent = reg.create();
    simcore::DrillComponent drill(0, 0, 0, 1); // tier 1 -> energyPerTick = 40
    drill.state = simcore::DrillState::MINING;
    drill.targetX = 1; drill.targetY = 0; drill.targetZ = 0;
    drill.miningTicksTotal = 1;
    drill.miningProgress = 1;
    reg.emplace<simcore::DrillComponent>(ent, drill);
    // No InventoryContainer -> legacy machine EnergyStorage path.
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 1000, 128, 128, 1,
                                        EnergyType::ELECTRICITY);

    sys.tick(0.05f);

    const auto& energy = reg.get<simcore::EnergyStorage>(ent);
    const auto& d = reg.get<simcore::DrillComponent>(ent);
    CHECK_EQ(energy.current, 960, "machine energy drained as fallback (tier 1: 40/tick)");
    CHECK_EQ(d.state, simcore::DrillState::MINING, "mining continues with machine energy");
    PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

void test_ecs_systems() {
    TEST(MachineRegistry_Yaml_lowercase_role);
    TEST(HeatTransferSystem_yaml_generator_to_furnace);
    TEST(SetBlockCASHandler_lazy_creates_pre_existing_machine);
    TEST(GeneratorSystem_burns_coal);
    TEST(GeneratorSystem_producer_maxInput_zero);
    TEST(GeneratorSystem_no_fuel_no_energy);
    TEST(GeneratorSystem_full_storage_skips);
    TEST(AdjacencyTransferSystem_adjacent_transfer);
    TEST(AdjacencyTransferSystem_non_adjacent_no_transfer);
    TEST(MachineSystem_idle_no_recipe);
    TEST(CreativeGeneratorSystem_fills_energy);
    TEST(BatteryBufferSystem_charges_tool);
    TEST(BatteryBufferSystem_empty_slot_noop);
    TEST(BatteryBufferSystem_full_tool_skips);
    TEST(MultiblockFormation_hatchIO);
    TEST(DrillSystem_drains_tool_energy);
    TEST(DrillSystem_insufficient_tool_energy_aborts);
    TEST(DrillSystem_falls_back_to_machine_energy);
}
