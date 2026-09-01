// PendingCraft orchestration tests (refactor-fluid-port-accounting 4.5.1-4.5.4):
// no/zero/partial acceptance keeps inputs and progress untouched, full
// acceptance commits exactly once, recurring charges gate advancement, and EU
// requirements ride the same reservation interface with load-time rejection of
// mismatched machine energy declarations.
#include <libgtnh-net/test/test.h>

#include "ECS/Systems/MachineSystem.h"
#include "Network/CraftReservationClient.h"
#include "Network/IEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "Network/clients/IoUringRouterClient.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/RecipeProgress.h"
#include "ECS/components/InventoryContainer.h"
#include "ECS/components/EnergyStorage.h"
#include "RecipeManager/RecipeManager.h"
#include "ItemRegistry.h"
#include "MachineRegistry.h"

#include <common/ItemId.h>
#include <common/Registry.h>
#include <common/ResourcePortClient.h>

#include <entt/entt.hpp>
#include <unistd.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern int g_tests, g_passed, g_failed;
void test_check(bool cond, const char* file, int line, const char* expr, const char* msg);

#ifndef CHECK_EQ
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#endif
#ifndef CHECK_GT
#define CHECK_GT(a, b, msg) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, msg)
#endif
#ifndef CHECK
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#endif

namespace {

struct CapturedRequest {
    std::uint64_t request_id = 0;
    gtnh::common::ResourceKind kind = gtnh::common::ResourceKind::FLUID;
    std::uint32_t resource_id = 0;
    std::int32_t amount = 0;
};

struct MockEventPublisher : simcore::IEventPublisher {
    void publishBlockAck(uint8_t, int32_t, int32_t, int32_t, uint16_t, uint8_t,
                         const char*, uint32_t, uint8_t) override {}
    void publishBlockDirective(uint8_t, uint16_t, int32_t, int32_t, int32_t,
                               uint32_t, uint8_t) override {}
    void publishBlockChangedEvent(int32_t, int32_t, int32_t, uint16_t, uint8_t,
                                  uint32_t, uint64_t) override {}
    void publishBlockEntityUpdate(int32_t, int32_t, int32_t, uint16_t,
                                  const std::vector<uint8_t>&, float, uint32_t,
                                  EnergyType, uint32_t, int, float,
                                  const std::vector<HatchUpdateData>* = nullptr,
                                  double = -1.0, double = -1.0) override {}
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
};

std::string makeTempFile(const std::string& content) {
    char tmpl[] = "/tmp/pending_craft_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return {};
    [[maybe_unused]] ssize_t wr = write(fd, content.data(), content.size());
    close(fd);
    return std::string(tmpl);
}

void ensureMachineRegistry() {
    static bool done = false;
    if (done && MachineRegistry::instance()) return;
    const char* consumers =
        "id,name,class,energy_in,tier,slots_in,slots_out,capacity,maxInput,maxOutput\n"
        "9101,test_steam_machine,macerator,STEAM,0,1,1,10000,32,0\n"
        "9102,test_eu_machine,chemical_reactor,ELECTRICITY,0,1,1,10000,32,0\n";
    const char* producers =
        "id,name,class,energy_out,energy_in,tier,slots_in,slots_out,capacity,maxInput,maxOutput\n";
    static std::string consumers_path = makeTempFile(consumers);
    static std::string producers_path = makeTempFile(producers);
    auto reg = MachineRegistry::Load(consumers_path.c_str(), producers_path.c_str());
    MachineRegistry::setInstance(reg.get());
    reg.release();
    done = true;
}

// One machine + one MachineSystem wired to a CraftReservationClient whose
// publisher captures the typed consume requests (no router, no PipeNetwork).
struct PendingCraftFixture {
    entt::registry reg;
    std::vector<CapturedRequest> requests;
    bool publish_enabled = true;
    std::shared_ptr<MockEventPublisher> events;
    std::shared_ptr<RecipeManager::RecipeManager> recipes;
    std::shared_ptr<simcore::CraftReservationClient> reservations;
    std::unique_ptr<simcore::MachineSystem> system;
    entt::entity machine{};
    std::uint16_t block_id = 0;
    const std::uint16_t input_item = ItemId::pack("0:11110:2");

    PendingCraftFixture(std::uint16_t machine_block,
                        EnergyType energy_type,
                        const std::string& recipe_yaml,
                        std::uint8_t machine_energy_in) {
        ensureMachineRegistry();
        RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");

        block_id = machine_block;
        events = std::make_shared<MockEventPublisher>();
        recipes = std::make_shared<RecipeManager::RecipeManager>();
        recipes->registerMachineClass(machine_block, recipeClassOf(machine_block), 0,
                                      machine_energy_in);
        CHECK(recipes->loadRecipesFromYamlFile(makeTempFile(recipe_yaml)),
              "fixture recipe YAML must load");

        auto publisher = [this](const char* topic,
                                const std::vector<std::uint8_t>& payload) {
            if (!publish_enabled) return false;
            (void)topic;
            gtnh::common::ResourceTransferRequest request;
            if (gtnh::common::ParseConsumeRequest(payload.data(), payload.size(),
                                                  &request)) {
                requests.push_back({request.request_id, request.resource_kind,
                                    request.resource_id, request.amount});
            }
            return true;
        };
        reservations = std::make_shared<simcore::CraftReservationClient>(reg, publisher);
        system = std::make_unique<simcore::MachineSystem>(
            reg, recipes, events, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, reservations);

        machine = reg.create();
        reg.emplace<simcore::MachineComponent>(machine, block_id, 0, 0, 0, 0, 0);
        reg.emplace<simcore::RecipeProgress>(machine);
        reg.emplace<simcore::EnergyStorage>(machine, 10000, 0, 32, 0, 0, energy_type);
        reg.emplace<simcore::InventoryContainer>(
            machine, 0, 1,
            std::vector<simcore::InventorySlot>{{input_item, 1, 0}});
    }

    static std::string recipeClassOf(std::uint16_t machine_block) {
        return machine_block == 9101 ? "macerator" : "chemical_reactor";
    }

    void tick() { system->tick(0.05f); }

    std::size_t requestCount() const { return requests.size(); }
    const CapturedRequest& lastRequest() const { return requests.back(); }

    void deliver(std::uint64_t request_id, std::int32_t accepted) {
        gtnh::common::ResourceTransferResponse response;
        response.request_id = request_id;
        response.accepted_amount = accepted;
        reservations->onConsumeResponse(response);
    }

    simcore::RecipeProgress& progress() { return reg.get<simcore::RecipeProgress>(machine); }
    simcore::EnergyStorage& energy() { return reg.get<simcore::EnergyStorage>(machine); }
    simcore::InventorySlot& inputSlot() { return reg.get<simcore::InventoryContainer>(machine).slots[0]; }
};

const std::uint32_t kSteamId = gtnh::common::steamItemId();

const char* kSteamRecipeYaml =
    "class: macerator\n"
    "recipes:\n"
    "  - name: test_pending_steam_recipe\n"
    "    energy_in: STEAM\n"
    "    inputs:\n"
    "      - { item: \"0:11110:2\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:11110:3\", count: 1 }\n"
    "    eu: 32\n"
    "    duration: 50\n"
    "    min_tier: 0\n"
    "    max_tier: 32767\n"
    "    resource_requirements:\n"
    "      - { kind: FLUID, resource_id: steam, amount: 32, tier: 0 }\n";

const char* kEuRecipeYaml =
    "class: chemical_reactor\n"
    "recipes:\n"
    "  - name: test_pending_eu_recipe\n"
    "    energy_in: ELECTRICITY\n"
    "    inputs:\n"
    "      - { item: \"0:11110:2\", count: 1 }\n"
    "    outputs:\n"
    "      - { item: \"0:11110:3\", count: 1 }\n"
    "    duration: 40\n"
    "    min_tier: 0\n"
    "    max_tier: 32767\n"
    "    resource_requirements:\n"
    "      - { kind: EU, amount: 32, tier: 0 }\n";

// -- 4.5.1: no request / zero acceptance / partial acceptance -----------------

void test_no_request_leaves_inputs_and_progress_unchanged() {
    PendingCraftFixture fx(9101, EnergyType::STEAM, kSteamRecipeYaml,
                           static_cast<std::uint8_t>(RecipeManager::EnergyType::STEAM));
    fx.publish_enabled = false; // no request can be published

    fx.tick();
    CHECK_EQ(fx.requestCount(), std::size_t(0), "no request published");
    CHECK(fx.progress().recipe_id.empty(), "no recipe started without a request");
    CHECK(!fx.progress().is_processing, "not processing without a request");
    CHECK_EQ(fx.inputSlot().count, std::uint8_t(1), "inputs untouched without a request");
    CHECK(fx.progress().pending_craft.has_value(), "craft stays pending");
    CHECK(!fx.progress().pending_craft->fullyAccepted(), "pending craft not accepted");
}

void test_zero_acceptance_leaves_inputs_and_progress_unchanged() {
    PendingCraftFixture fx(9101, EnergyType::STEAM, kSteamRecipeYaml,
                           static_cast<std::uint8_t>(RecipeManager::EnergyType::STEAM));

    fx.tick();
    CHECK_EQ(fx.requestCount(), std::size_t(1), "one consume request published");
    CHECK_EQ(fx.lastRequest().resource_id, kSteamId, "request carries canonical steam id");
    CHECK_EQ(fx.lastRequest().amount, 32, "request carries the requirement amount");

    fx.deliver(fx.lastRequest().request_id, 0); // zero acceptance
    fx.tick();
    CHECK(fx.progress().recipe_id.empty(), "zero acceptance never starts the recipe");
    CHECK(!fx.progress().is_processing, "zero acceptance never sets processing");
    CHECK_EQ(fx.inputSlot().count, std::uint8_t(1), "zero acceptance consumes no inputs");
    CHECK_EQ(fx.energy().current, 0, "zero acceptance credits nothing");
}

void test_partial_acceptance_leaves_inputs_and_progress_unchanged() {
    PendingCraftFixture fx(9101, EnergyType::STEAM, kSteamRecipeYaml,
                           static_cast<std::uint8_t>(RecipeManager::EnergyType::STEAM));

    fx.tick();
    fx.deliver(fx.lastRequest().request_id, 10); // partial: 10 of 32
    fx.tick();
    CHECK(fx.progress().recipe_id.empty(), "partial acceptance never starts the recipe");
    CHECK_EQ(fx.inputSlot().count, std::uint8_t(1), "partial acceptance consumes no inputs");
    CHECK_EQ(fx.energy().current, 10, "partial acceptance credits only the accepted part");
    CHECK(fx.progress().pending_craft->partial(), "reservation records partial acceptance");

    // Bounded retry: a fresh request for the remaining amount goes out.
    const std::size_t before = fx.requestCount();
    for (int i = 0; i < 12; ++i) fx.tick(); // past the backoff deadline
    CHECK_GT(fx.requestCount(), before, "retry re-requests the remaining amount");
    CHECK_EQ(fx.lastRequest().amount, 22, "retry requests only the remaining amount");
}

// -- 4.5.2: full acceptance commits inputs exactly once -----------------------

void test_full_acceptance_commits_inputs_exactly_once() {
    PendingCraftFixture fx(9101, EnergyType::STEAM, kSteamRecipeYaml,
                           static_cast<std::uint8_t>(RecipeManager::EnergyType::STEAM));

    fx.tick();
    const auto request_id = fx.lastRequest().request_id;
    fx.deliver(request_id, 32); // full acceptance
    fx.tick();                  // commit tick

    CHECK_EQ(fx.inputSlot().count, std::uint8_t(0), "inputs consumed at commit");
    CHECK(!fx.progress().recipe_id.empty(), "recipe started at commit");
    CHECK(fx.progress().is_processing, "processing after commit");
    CHECK(!fx.progress().pending_craft.has_value(), "pending craft cleared at commit");
    // The credited buffer covered the first tick: 32 charged, 32 debited.
    CHECK_EQ(fx.energy().current, 0, "first per-tick charge debited from the buffer");
    CHECK_EQ(fx.progress().remaining_ticks, std::uint32_t(49),
             "progress started and advanced exactly one tick");

    // A duplicate response for the same request id must do nothing.
    fx.deliver(request_id, 32);
    CHECK_EQ(fx.energy().current, 0, "duplicate response never double-credits");
    CHECK_EQ(fx.inputSlot().count, std::uint8_t(0), "duplicate response never re-consumes");
}

// -- 4.5.3: recurring charge before progress advancement ----------------------

void test_recurring_charge_gates_progress_advancement() {
    PendingCraftFixture fx(9101, EnergyType::STEAM, kSteamRecipeYaml,
                           static_cast<std::uint8_t>(RecipeManager::EnergyType::STEAM));

    fx.tick();
    fx.deliver(fx.lastRequest().request_id, 32);
    fx.tick(); // commit + first tick advance
    CHECK_EQ(fx.progress().remaining_ticks, std::uint32_t(49), "started at 49");

    // Buffer empty: the next tick must request a charge and NOT advance.
    fx.tick();
    CHECK_EQ(fx.lastRequest().amount, 32, "recurring charge requested for the per-tick amount");
    CHECK_EQ(fx.progress().remaining_ticks, std::uint32_t(49),
             "no advancement before the charge is accepted");

    // Zero acceptance: still no advancement.
    fx.deliver(fx.lastRequest().request_id, 0);
    fx.tick();
    CHECK_EQ(fx.progress().remaining_ticks, std::uint32_t(49),
             "zero-accepted charge never advances progress");

    // Full acceptance credits the buffer; the NEXT tick debits and advances.
    fx.tick();
    const auto charge_id = fx.lastRequest().request_id;
    CHECK_EQ(fx.lastRequest().amount, 32, "re-requests the charge after zero acceptance");
    fx.deliver(charge_id, 32);
    CHECK_EQ(fx.energy().current, 32, "accepted charge credits the buffer");
    CHECK_EQ(fx.progress().remaining_ticks, std::uint32_t(49),
             "credit alone does not advance progress");
    fx.tick();
    CHECK_EQ(fx.progress().remaining_ticks, std::uint32_t(48),
             "progress advances exactly once per accepted charge");
    CHECK_EQ(fx.energy().current, 0, "charge debited with the advancement");
}

// -- 4.5.4: EU through the same interface + mismatched declarations rejected --

void test_eu_requirement_uses_same_reservation_interface() {
    PendingCraftFixture fx(9102, EnergyType::ELECTRICITY, kEuRecipeYaml,
                           static_cast<std::uint8_t>(RecipeManager::EnergyType::ELECTRICITY));

    fx.tick();
    CHECK_EQ(fx.requestCount(), std::size_t(1), "EU requirement publishes one request");
    CHECK_EQ(fx.lastRequest().kind, gtnh::common::ResourceKind::EU, "EU kind on the wire");
    CHECK_EQ(fx.lastRequest().resource_id, std::uint32_t(0), "energy channels carry no resource id");
    CHECK_EQ(fx.lastRequest().amount, 32, "EU amount from the requirement");

    fx.deliver(fx.lastRequest().request_id, 32);
    fx.tick();
    CHECK_EQ(fx.inputSlot().count, std::uint8_t(0), "EU commit consumes inputs");
    CHECK(!fx.progress().recipe_id.empty(), "EU commit starts progress");
    CHECK_EQ(fx.progress().remaining_ticks, std::uint32_t(39), "EU recipe ticks once");
}

void test_mismatched_energy_declarations_rejected_at_load() {
    RecipeManager::ItemRegistry::instance().loadFromCSV(DATA_DIR "/registry/items.csv");
    auto recipes = std::make_shared<RecipeManager::RecipeManager>();
    recipes->loadMachinesFromYaml(DATA_DIR "/registry/machines.yaml");

    // kind contradicts energy_in: STEAM machine recipe demanding EU.
    const std::string kind_mismatch =
        "class: compressor\n"
        "recipes:\n"
        "  - name: test_kind_mismatch\n"
        "    energy_in: STEAM\n"
        "    inputs:\n"
        "      - { item: \"0:11110:2\", count: 1 }\n"
        "    outputs:\n"
        "      - { item: \"0:11110:3\", count: 1 }\n"
        "    duration: 40\n"
        "    resource_requirements:\n"
        "      - { kind: EU, amount: 32, tier: 0 }\n";
    CHECK(!recipes->loadRecipesFromYamlFile(makeTempFile(kind_mismatch)),
          "requirement kind contradicting energy_in must fail validation");

    // kind not consumed by any variant of the machine class: compressor is
    // STEAM-only in machines.yaml, so an EU requirement has no provider.
    const std::string class_mismatch =
        "class: compressor\n"
        "recipes:\n"
        "  - name: test_class_mismatch\n"
        "    inputs:\n"
        "      - { item: \"0:11110:2\", count: 1 }\n"
        "    outputs:\n"
        "      - { item: \"0:11110:3\", count: 1 }\n"
        "    duration: 40\n"
        "    resource_requirements:\n"
        "      - { kind: EU, amount: 32, tier: 0 }\n";
    CHECK(!recipes->loadRecipesFromYamlFile(makeTempFile(class_mismatch)),
          "requirement kind without a matching machine variant must fail validation");
}

// -- 4.2.4: cancellation drops outstanding requests ---------------------------

void test_cancel_drops_outstanding_requests() {
    PendingCraftFixture fx(9101, EnergyType::STEAM, kSteamRecipeYaml,
                           static_cast<std::uint8_t>(RecipeManager::EnergyType::STEAM));

    fx.tick();
    const auto request_id = fx.lastRequest().request_id;
    CHECK(fx.progress().pending_craft.has_value(), "pending craft exists before cancel");

    fx.reservations->cancel(fx.machine, "test");
    CHECK(!fx.progress().pending_craft.has_value(), "cancel clears the pending craft");

    // The response to the cancelled request finds no outstanding entry.
    fx.deliver(request_id, 32);
    CHECK_EQ(fx.energy().current, 0, "response after cancel is ignored");
}

} // namespace

void test_pending_craft() {
    printf("  TEST: no_request_leaves_inputs_and_progress_unchanged\n");
    test_no_request_leaves_inputs_and_progress_unchanged();
    printf("  TEST: zero_acceptance_leaves_inputs_and_progress_unchanged\n");
    test_zero_acceptance_leaves_inputs_and_progress_unchanged();
    printf("  TEST: partial_acceptance_leaves_inputs_and_progress_unchanged\n");
    test_partial_acceptance_leaves_inputs_and_progress_unchanged();
    printf("  TEST: full_acceptance_commits_inputs_exactly_once\n");
    test_full_acceptance_commits_inputs_exactly_once();
    printf("  TEST: recurring_charge_gates_progress_advancement\n");
    test_recurring_charge_gates_progress_advancement();
    printf("  TEST: eu_requirement_uses_same_reservation_interface\n");
    test_eu_requirement_uses_same_reservation_interface();
    printf("  TEST: mismatched_energy_declarations_rejected_at_load\n");
    test_mismatched_energy_declarations_rejected_at_load();
    printf("  TEST: cancel_drops_outstanding_requests\n");
    test_cancel_drops_outstanding_requests();
}
