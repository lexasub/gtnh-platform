// Converter test (openspec refactor-fluid-port-accounting 2.4.4): the heat
// boiler registers an HU sink and a FLUID steam source on the same owner and
// the two roles stay independent — re-registering or replacing one port never
// disturbs the other, and repeated ticks do not create duplicates.
//
// Receiver-side ordering (HU-before-FLUID vs FLUID-before-HU) is covered at
// the record level: the port factories are pure, so each published record is
// self-contained and the receiver converges to the same state in either
// order. Routing typed registrations into per-domain graphs is task 2.3.2.
#include <libgtnh-net/test/test.h>

#include <common/ItemId.h>
#include <common/Registry.h>
#include <common/ResourcePort.h>
#include <common/ResourcePortClient.h>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "ECS/Systems/BoilerPorts.h"
#include "ECS/Systems/BoilerSystem.h"
#include "ECS/Systems/HeatConstants.h"
#include "ECS/components/SteamOutputComponent.h"

extern int g_tests, g_passed, g_failed;
void test_check(bool cond, const char* file, int line, const char* expr, const char* msg);

#ifndef CHECK_EQ
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#endif
#ifndef CHECK_NE
#define CHECK_NE(a, b, msg) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, msg)
#endif
#ifndef CHECK
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#endif
#ifndef PASS
#define PASS() do { ++g_passed; } while(0)
#endif

namespace {

using gtnh::common::PortId;
using gtnh::common::ResourceKind;
using gtnh::common::ResourcePort;
using gtnh::common::PortRole;

// Records every typed registration and models the receiver-side dedupe
// contract: `latest` is keyed by port identity (owner, port_id) so repeated
// publishes of the same (owner, kind, port, epoch) collapse into one entry.
class RecordingPortClient : public gtnh::common::IResourcePortClient {
 public:
  int register_calls = 0;
  std::map<std::pair<std::uint64_t, PortId>, ResourcePort> latest;
  std::map<std::pair<std::uint64_t, PortId>, std::uint32_t> latest_resource_ids;

  bool PublishPortRegister(const ResourcePort& port,
                           std::uint32_t resource_id) override {
    ++register_calls;
    latest[{port.owner_id, port.port_id}] = port;
    latest_resource_ids[{port.owner_id, port.port_id}] = resource_id;
    return true;
  }
  bool PublishPortRemove(std::uint64_t, ResourceKind, PortId,
                         std::uint64_t) override {
    return true;
  }
  bool PublishDrainRequest(
      const gtnh::common::ResourceTransferRequest&) override {
    return true;
  }
  bool PublishConsumeRequest(
      const gtnh::common::ResourceTransferRequest&) override {
    return true;
  }
};

// BoilerSystem publishes UI state every tick; no-op stand-in.
class NullEventPublisher : public simcore::IEventPublisher {
 public:
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

entt::entity MakeBoiler(entt::registry& reg) {
  auto ent = reg.create();
  reg.emplace<simcore::MachineComponent>(ent, ItemId::pack("1110:01:1"), 0,
                                         5, 6, 7, 1);
  reg.emplace<simcore::EnergyStorage>(ent, 10000, 500, 0, 32, 1,
                                      EnergyType::HEAT);
  simcore::HeatIntakeComponent heat;
  heat.heat_stored = 500;
  heat.heat_capacity = 1000;
  reg.emplace<simcore::HeatIntakeComponent>(ent, heat);
  simcore::SteamOutputComponent steam;
  steam.steam_capacity = 1000;
  reg.emplace<simcore::SteamOutputComponent>(ent, steam);
  return ent;
}

const ResourcePort* FindPort(const RecordingPortClient& client,
                             std::uint64_t owner, PortId port_id) {
  const auto it = client.latest.find({owner, port_id});
  return it != client.latest.end() ? &it->second : nullptr;
}

}  // namespace

// 2.4.1: deterministic, distinct, stable; owner 0 is a valid owner.
static void test_BoilerPorts_scheme_distinct_and_stable() {
  CHECK_EQ(simcore::BoilerPorts::kBoilerHuSinkPortId,
           simcore::BoilerPorts::MakeMachinePortId(1, 1),
           "HU sink slot must be (class 1 << 8) | 1");
  CHECK_EQ(simcore::BoilerPorts::kBoilerSteamSourcePortId,
           simcore::BoilerPorts::MakeMachinePortId(1, 2),
           "steam source slot must be (class 1 << 8) | 2");
  CHECK_NE(simcore::BoilerPorts::kBoilerHuSinkPortId,
           simcore::BoilerPorts::kBoilerSteamSourcePortId,
           "the two boiler ports must have distinct port ids");

  // Pure factories: identical arguments produce identical records, so the
  // port id/record is stable across ticks and restarts.
  const auto a = simcore::BoilerPorts::MakeHuSinkPort(7, 1, 2, 3, 1000, 100, 1);
  const auto b = simcore::BoilerPorts::MakeHuSinkPort(7, 1, 2, 3, 1000, 100, 1);
  CHECK(a.registrationKey() == b.registrationKey(),
        "same inputs must yield the same registration key");
  CHECK_EQ(a.capacity, b.capacity, "capacity must be deterministic");
  CHECK_EQ(a.rate, b.rate, "rate must be deterministic");

  // Design section 2: owner_id 0 is valid.
  const auto zero = simcore::BoilerPorts::MakeHuSinkPort(0, 0, 0, 0, 100, 1, 1);
  CHECK(zero.valid(), "port with owner 0 must be valid");
  CHECK_EQ(zero.owner_id, std::uint64_t(0), "owner 0 must be preserved");
  PASS();
}

// 2.4.2/2.4.4: same owner, two ports, roles do not bleed. Wire-level
// round-trip of the register payload is owned by the shared contract layer
// (common/ResourcePortClient.h); its serializers currently skip the
// top-level FlatBufferBuilder::Finish (see integration notes for item 2.4),
// so this test pins the record-level contract only.
static void test_BoilerPorts_roles_independent_same_owner() {
  const std::uint64_t owner = 42;
  const auto hu = simcore::BoilerPorts::MakeHuSinkPort(owner, 5, 6, 7, 1000,
                                                       100, 1);
  const auto steam = simcore::BoilerPorts::MakeSteamSourcePort(
      owner, 5, 6, 7, 1000, 1, 1);

  CHECK_EQ(hu.owner_id, steam.owner_id, "both ports share one owner");
  CHECK_NE(hu.port_id, steam.port_id, "ports must be distinct");
  CHECK(hu.resource_kind == ResourceKind::HU, "HU sink kind");
  CHECK(hu.role == PortRole::SINK, "HU port must be a SINK");
  CHECK(steam.resource_kind == ResourceKind::FLUID, "steam port kind");
  CHECK(steam.role == PortRole::SOURCE, "steam port must be a SOURCE");
  CHECK_EQ(hu.capacity, 1000, "HU sink capacity from heat buffer");
  CHECK_EQ(steam.capacity, 1000, "steam source capacity from steam buffer");
  CHECK_EQ(hu.face_policy, gtnh::common::FacePolicy::ALL_FACES,
           "HU sink face policy");
  CHECK_EQ(steam.face_mask, 0x3f, "steam source covers all six faces");
  PASS();
}

// 2.4.3: repeated ticks republish both ports at the same epoch — the receiver
// sees 6 registrations but only 2 distinct ports, no duplicates, no drift.
static void test_BoilerSystem_publishes_both_ports_idempotently() {
  entt::registry reg;
  auto events = std::make_shared<NullEventPublisher>();
  auto port_client = std::make_shared<RecordingPortClient>();
  simcore::BoilerSystem sys(reg, events, nullptr, nullptr, port_client,
                              gtnh::common::steamItemId());

  const auto ent = MakeBoiler(reg);
  const std::uint64_t owner = static_cast<std::uint64_t>(ent);

  sys.tick(0.05f);
  sys.tick(0.05f);
  sys.tick(0.05f);

  CHECK_EQ(port_client->register_calls, 6,
           "two ports published per tick for three ticks");
  CHECK_EQ(port_client->latest.size(), size_t(2),
           "repeated ticks must not create duplicate ports");

  const ResourcePort* hu =
      FindPort(*port_client, owner, simcore::BoilerPorts::kBoilerHuSinkPortId);
  const ResourcePort* steam = FindPort(*port_client, owner,
                                       simcore::BoilerPorts::kBoilerSteamSourcePortId);
  CHECK(hu != nullptr, "HU sink must be registered");
  CHECK(steam != nullptr, "steam source must be registered");
  if (hu == nullptr || steam == nullptr) return;

  CHECK_EQ(hu->owner_id, owner, "HU sink owner is the boiler entity");
  CHECK_EQ(steam->owner_id, owner, "steam source owner is the boiler entity");
  CHECK(hu->resource_kind == ResourceKind::HU && hu->role == PortRole::SINK,
        "HU sink role must not bleed into the fluid port");
  CHECK(steam->resource_kind == ResourceKind::FLUID &&
            steam->role == PortRole::SOURCE,
        "steam source role must not bleed into the heat port");
  CHECK_EQ(hu->epoch, std::uint64_t(1), "stable epoch across ticks");
  CHECK_EQ(steam->epoch, std::uint64_t(1), "stable epoch across ticks");
  CHECK_EQ(hu->x, 5, "HU sink position from MachineComponent");
  CHECK_EQ(steam->z, 7, "steam source position from MachineComponent");
  CHECK_EQ(hu->capacity, 1000, "HU sink capacity = heat_capacity");
  CHECK_EQ(hu->rate, simcore::HeatConstants::HEAT_SINK_REPLENISH_TARGET,
           "HU sink rate = replenish pull target");
  CHECK_EQ(steam->capacity, 1000, "steam capacity from SteamOutputComponent");
  CHECK_EQ(steam->rate, simcore::HeatConstants::CONVERSION_RATE,
           "steam rate = per-tick conversion cap");

  const auto hu_key = std::make_pair(owner, simcore::BoilerPorts::kBoilerHuSinkPortId);
  const auto steam_key =
      std::make_pair(owner, simcore::BoilerPorts::kBoilerSteamSourcePortId);
  CHECK_EQ(port_client->latest_resource_ids[hu_key], std::uint32_t(0),
           "HU carries no resource id");
  CHECK_EQ(port_client->latest_resource_ids[steam_key],
           gtnh::common::steamItemId(),
           "steam source advertises the packed steam ItemId");
  PASS();
}

// 2.4.4: epoch replacement works per port — replacing one port bumps only
// that port's epoch and leaves the other port's record untouched.
static void test_BoilerSystem_epoch_replacement_per_port() {
  entt::registry reg;
  auto events = std::make_shared<NullEventPublisher>();
  auto port_client = std::make_shared<RecordingPortClient>();
  simcore::BoilerSystem sys(reg, events, nullptr, nullptr, port_client,
                              gtnh::common::steamItemId());

  const auto ent = MakeBoiler(reg);
  const std::uint64_t owner = static_cast<std::uint64_t>(ent);
  const PortId hu_id = simcore::BoilerPorts::kBoilerHuSinkPortId;
  const PortId steam_id = simcore::BoilerPorts::kBoilerSteamSourcePortId;

  sys.tick(0.05f);
  const ResourcePort hu_baseline = *FindPort(*port_client, owner, hu_id);
  const ResourcePort steam_baseline = *FindPort(*port_client, owner, steam_id);

  CHECK_EQ(sys.replacePort(owner, hu_id), std::uint64_t(2),
           "replacement = epoch + 1");
  sys.tick(0.05f);

  const ResourcePort* hu = FindPort(*port_client, owner, hu_id);
  const ResourcePort* steam = FindPort(*port_client, owner, steam_id);
  CHECK(hu != nullptr && steam != nullptr, "both ports still registered");
  if (hu == nullptr || steam == nullptr) return;

  CHECK_EQ(hu->epoch, std::uint64_t(2), "HU sink epoch advanced");
  CHECK_EQ(steam->epoch, steam_baseline.epoch,
           "steam source epoch must not move when HU is replaced");
  CHECK_EQ(steam->registrationKey(), steam_baseline.registrationKey(),
           "steam source record untouched by HU replacement");
  CHECK_EQ(hu->port_id, hu_baseline.port_id,
           "replacement keeps the port id stable");

  CHECK_EQ(sys.replacePort(owner, steam_id), std::uint64_t(2),
           "steam replacement is independent of HU history");
  sys.tick(0.05f);
  CHECK_EQ(FindPort(*port_client, owner, steam_id)->epoch, std::uint64_t(2),
           "steam source epoch advanced");
  CHECK_EQ(FindPort(*port_client, owner, hu_id)->epoch, std::uint64_t(2),
           "HU sink keeps its replaced epoch");
  CHECK_EQ(port_client->latest.size(), size_t(2),
           "replacement overwrites in place — still exactly two ports");
  PASS();
}

// 2.4.3: registration is order-independent — HU-then-FLUID and
// FLUID-then-HU end in the same receiver state, and partial arrival
// (one port first, the other later) converges to the same final records.
static void test_BoilerPorts_registration_order_independent() {
  const auto hu_a = simcore::BoilerPorts::MakeHuSinkPort(9, 1, 1, 1, 1000, 100, 1);
  const auto steam_a =
      simcore::BoilerPorts::MakeSteamSourcePort(9, 1, 1, 1, 1000, 1, 1);
  const auto hu_b = simcore::BoilerPorts::MakeHuSinkPort(9, 1, 1, 1, 1000, 100, 1);
  const auto steam_b =
      simcore::BoilerPorts::MakeSteamSourcePort(9, 1, 1, 1, 1000, 1, 1);

  std::map<PortId, ResourcePort> hu_first;
  hu_first[hu_a.port_id] = hu_a;
  hu_first[steam_a.port_id] = steam_a;
  std::map<PortId, ResourcePort> steam_first;
  steam_first[steam_b.port_id] = steam_b;
  steam_first[hu_b.port_id] = hu_b;

  CHECK_EQ(hu_first.size(), steam_first.size(),
           "both orders register exactly two ports");
  for (const auto& [port_id, port] : hu_first) {
    const auto it = steam_first.find(port_id);
    CHECK(it != steam_first.end(), "both orders must contain every port");
    if (it == steam_first.end()) continue;
    CHECK(it->second.registrationKey() == port.registrationKey(),
          "registration key must not depend on publication order");
    CHECK(it->second.role == port.role && it->second.resource_kind == port.resource_kind,
          "role/kind must not depend on publication order");
    CHECK_EQ(it->second.capacity, port.capacity,
             "capacity must not depend on publication order");
    CHECK_EQ(it->second.rate, port.rate,
             "rate must not depend on publication order");
  }

  // Partial arrival: HU alone, then FLUID alone, converges to the same state.
  RecordingPortClient partial;
  partial.PublishPortRegister(hu_a, 0);
  CHECK_EQ(partial.latest.size(), size_t(1),
           "one port alone registers without the other");
  partial.PublishPortRegister(steam_a, gtnh::common::steamItemId());
  CHECK_EQ(partial.latest.size(), size_t(2),
           "late second port completes the converter registration");
  const auto hu_key = std::make_pair(hu_a.owner_id, hu_a.port_id);
  const auto steam_key = std::make_pair(steam_a.owner_id, steam_a.port_id);
  CHECK(partial.latest[hu_key].registrationKey() ==
            hu_first[hu_a.port_id].registrationKey(),
        "partial arrival converges to the same HU record");
  CHECK(partial.latest[steam_key].registrationKey() ==
            hu_first[steam_a.port_id].registrationKey(),
        "partial arrival converges to the same steam record");
  PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

void test_boiler_ports() {
    TEST(BoilerPorts_scheme_distinct_and_stable);
    TEST(BoilerPorts_roles_independent_same_owner);
    TEST(BoilerSystem_publishes_both_ports_idempotently);
    TEST(BoilerSystem_epoch_replacement_per_port);
    TEST(BoilerPorts_registration_order_independent);
}
