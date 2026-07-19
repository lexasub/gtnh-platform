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
#include "ECS/components/MachineComponent.h"
#include "ECS/components/RecipeProgress.h"
#include "ECS/components/InventoryContainer.h"
#include "ECS/components/EnergyStorage.h"
#include "ECS/components/Position.h"
#include "ECS/Systems/GeneratorSystem.h"
#include "ECS/Systems/HeatTransferSystem.h"
#include "ECS/Systems/MachineSystem.h"
#include "ECS/Systems/CreativeGeneratorSystem.h"
#include "ECS/Systems/BoilerSystem.h"
#include "ECS/Systems/BatteryBufferSystem.h"
#include "Network/IEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "MachineRegistry.h"
#include "RecipeManager/RecipeManager.h"

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
    write(fd, content.data(), content.size());
    close(fd);
    return std::string(tmpl);
}

struct MockEventPublisher : simcore::IEventPublisher {
    int block_ack_count = 0;
    int block_changed_count = 0;
    int block_entity_update_count = 0;
    int last_x = 0, last_y = 0, last_z = 0;
    uint16_t last_machine_id = 0;
    float last_progress = 0;
    uint32_t last_energy = 0;

    void publishBlockAck(uint8_t, int32_t x, int32_t y, int32_t z,
                         uint16_t, uint8_t, const char*, uint32_t) override {
        block_ack_count++;
        last_x = x; last_y = y; last_z = z;
    }
    void publishBlockChangedEvent(int32_t, int32_t, int32_t,
                                  uint16_t, uint8_t) override {
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
                                   float) override {
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

static void test_GeneratorSystem_burns_coal() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem sys(reg, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 46, 0, 100, 64, 100, 1);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 0, 128, 128, 0, EnergyType::HEAT);
    simcore::InventoryContainer container(0, 1, {{44, 1, 0}});
    reg.emplace<simcore::InventoryContainer>(ent, container);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_GT(energy.current, 0, "generator should produce energy from coal");
    CHECK_GT(events->block_entity_update_count, 0, "should publish BlockEntityUpdate");
    CHECK_EQ(events->last_machine_id, 46, "machine_id should be heat_generator");

    PASS();
}

static void test_GeneratorSystem_no_fuel_no_energy() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem sys(reg, events, pipeClient);

    auto ent = reg.create();
    reg.emplace<simcore::MachineComponent>(ent, 46, 0, 101, 64, 101, 2);
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
    reg.emplace<simcore::MachineComponent>(ent, 46, 0, 102, 64, 102, 3);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 10000, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::InventoryContainer>(ent);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_EQ(energy.current, 10000, "full storage should stay full");

    PASS();
}

static void test_HeatTransferSystem_adjacent_transfer() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem genSys(reg, events, pipeClient);
    simcore::HeatTransferSystem heatSys(reg, *MachineRegistry::instance(), events);

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
    heatSys.tick(0.05f);

    auto& genEnergy = reg.get<simcore::EnergyStorage>(gen);
    auto& furnEnergy = reg.get<simcore::EnergyStorage>(furn);

    CHECK_GT(furnEnergy.current, 0, "adjacent furnace should receive heat");
    CHECK_LT(genEnergy.current, 5000, "generator energy decreased after transfer");

    PASS();
}

static void test_HeatTransferSystem_non_adjacent_no_transfer() {
    setupMachineRegistry();
    entt::registry reg;
    auto events = std::make_shared<MockEventPublisher>();
    auto pipeClient = std::make_shared<simcore::PipeEnergyClient>(std::make_shared<simcore::IoUringRouterClient>());
    simcore::GeneratorSystem genSys(reg, events, pipeClient);
    simcore::HeatTransferSystem heatSys(reg, *MachineRegistry::instance(), events);

    auto gen = reg.create();
    reg.emplace<simcore::MachineComponent>(gen, 46, 0, 0, 0, 0, 20);
    reg.emplace<simcore::EnergyStorage>(gen, 10000, 5000, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::Position>(gen, 0, 0, 0);
    reg.emplace<simcore::InventoryContainer>(gen);

    auto furn = reg.create();
    reg.emplace<simcore::MachineComponent>(furn, 36, 0, 10, 0, 0, 21);
    reg.emplace<simcore::EnergyStorage>(furn, 10000, 0, 128, 128, 0, EnergyType::HEAT);
    reg.emplace<simcore::Position>(furn, 10, 0, 0);

    heatSys.tick(0.05f);

    auto& furnEnergy = reg.get<simcore::EnergyStorage>(furn);
    CHECK_EQ(furnEnergy.current, 0, "non-adjacent furnace gets no heat");

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
    reg.emplace<simcore::MachineComponent>(ent, 63, 0, 300, 50, 300, 40);
    reg.emplace<simcore::EnergyStorage>(ent, 10000, 0, 0, 0, 10, EnergyType::ELECTRICITY);

    sys.tick(0.05f);
    auto& energy = reg.get<simcore::EnergyStorage>(ent);

    CHECK_GT(energy.current, 0, "creative generator fills energy");
    CHECK_GT(events->block_entity_update_count, 0, "publishes update");

    PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

void test_ecs_systems() {
    TEST(GeneratorSystem_burns_coal);
    TEST(GeneratorSystem_no_fuel_no_energy);
    TEST(GeneratorSystem_full_storage_skips);
    TEST(HeatTransferSystem_adjacent_transfer);
    TEST(HeatTransferSystem_non_adjacent_no_transfer);
    TEST(MachineSystem_idle_no_recipe);
    TEST(CreativeGeneratorSystem_fills_energy);
    TEST(BatteryBufferSystem_charges_tool);
    TEST(BatteryBufferSystem_empty_slot_noop);
    TEST(BatteryBufferSystem_full_tool_skips);
}
