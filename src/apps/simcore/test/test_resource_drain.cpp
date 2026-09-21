// Owner-side typed resource drain tests (refactor-fluid-port-accounting
// 3.2.1-3.2.4 + 2.5.2/2.5.4 simcore side): replay safety, zero/short
// acceptance matrix, and owner-destruction cleanup.
#include <engine/net/test/test.h>

#include "Network/ResourceDrainHandler.h"
#include <engine/sim/components/FluidStorage.h>
#include <engine/sim/components/SteamOutputComponent.h>

#include <engine/registry/Registry.h>
#include <common/ResourcePortClient.h>
#include <apps/pipe_network/PipeConsumeTransactions.h>

#include <entt/entt.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern int g_tests, g_passed, g_failed;
void test_check(bool cond, const char* file, int line, const char* expr, const char* msg);

#ifndef CHECK_EQ
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#endif

namespace {

using gtnh::common::kTopicResourceDrainRequest;
using gtnh::common::kTopicResourceDrainResponse;
using gtnh::common::kTopicResourcePortRegister;
using gtnh::common::kTopicResourcePortRemove;
using gtnh::common::ParseDrainResponse;
using gtnh::common::ParsePortRemove;
using gtnh::common::PortId;
using gtnh::common::PortRole;
using gtnh::common::ResourceKind;
using gtnh::common::ResourcePort;
using gtnh::common::ResourceTransferRequest;
using gtnh::common::ResourceTransferResponse;

// gtnh::common::Serialize* helpers leave the FlatBufferBuilder unfinished
// (table-builder Finish() returns the root offset without finishing the
// buffer), so the fixtures build payloads locally with the two-stage Finish.
std::vector<std::uint8_t> serializePortRegister(const ResourcePort& port,
                                                std::uint32_t resource_id) {
    flatbuffers::FlatBufferBuilder fbb;
    const Protocol::Vec3i pos(port.x, port.y, port.z);
    Protocol::ResourcePortRegisterBuilder builder(fbb);
    builder.add_port_id(port.port_id);
    builder.add_owner_id(port.owner_id);
    builder.add_resource_kind(gtnh::common::ToWire(port.resource_kind));
    builder.add_resource_id(resource_id);
    builder.add_role(gtnh::common::ToWire(port.role));
    builder.add_pos(&pos);
    builder.add_capacity(port.capacity);
    builder.add_rate(port.rate);
    builder.add_epoch(port.epoch);
    builder.add_face_policy(gtnh::common::ToWire(port.face_policy));
    builder.add_face_mask(port.face_mask);
    fbb.Finish(builder.Finish());
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

std::vector<std::uint8_t> serializeDrainRequest(const ResourceTransferRequest& request) {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::ResourceDrainRequestBuilder builder(fbb);
    builder.add_request_id(request.request_id);
    builder.add_port_id(request.port_id);
    builder.add_resource_kind(gtnh::common::ToWire(request.resource_kind));
    builder.add_resource_id(request.resource_id);
    builder.add_amount(request.amount);
    fbb.Finish(builder.Finish());
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

std::vector<std::uint8_t> serializePortRemove(std::uint64_t owner_id, ResourceKind kind,
                                              PortId port_id, std::uint64_t epoch) {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::ResourcePortRemoveBuilder builder(fbb);
    builder.add_port_id(port_id);
    builder.add_owner_id(owner_id);
    builder.add_epoch(epoch);
    builder.add_resource_kind(gtnh::common::ToWire(kind));
    fbb.Finish(builder.Finish());
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

struct CapturedTraffic {
    std::vector<gtnh::common::ResourceTransferResponse> drain_responses;
    std::vector<gtnh::common::ResourcePortRegistrationKey> port_removes;

    void clear() { drain_responses.clear(); port_removes.clear(); }
};

gtnh::common::ResourcePort makeFluidPort(PortId port_id, std::uint64_t owner,
                                         PortRole role, std::int32_t rate,
                                         std::int32_t capacity) {
    ResourcePort port;
    port.port_id = port_id;
    port.owner_id = owner;
    port.resource_kind = ResourceKind::FLUID;
    port.role = role;
    port.x = 1;
    port.y = 2;
    port.z = 3;
    port.capacity = capacity;
    port.rate = rate;
    port.epoch = 1;
    return port;
}

std::vector<std::uint8_t> drainPayload(std::uint64_t request_id, PortId port_id,
                                       ResourceKind kind, std::uint32_t resource_id,
                                       std::int32_t amount) {
    ResourceTransferRequest request;
    request.request_id = request_id;
    request.port_id = port_id;
    request.resource_kind = kind;
    request.resource_id = resource_id;
    request.amount = amount;
    return serializeDrainRequest(request);
}

// One handler + one registry + captured traffic. A dummy entity is created
// first so the machine entity id is nonzero and owner id 0 is guaranteed to
// be an invalid entity.
struct DrainFixture {
    entt::registry reg;
    CapturedTraffic traffic;
    std::shared_ptr<simcore::ResourceDrainHandler> handler;
    entt::entity machine{};

    DrainFixture() {
        auto sink = [this](const char* topic, const std::vector<std::uint8_t>& payload) {
            if (std::string(topic) == kTopicResourceDrainResponse) {
                gtnh::common::ResourceTransferResponse response;
                if (ParseDrainResponse(payload.data(), payload.size(), &response)) {
                    traffic.drain_responses.push_back(response);
                }
            } else if (std::string(topic) == kTopicResourcePortRemove) {
                gtnh::common::ResourcePortRegistrationKey key;
                if (ParsePortRemove(payload.data(), payload.size(), &key)) {
                    traffic.port_removes.push_back(key);
                }
            }
            return true;
        };
        handler = std::make_shared<simcore::ResourceDrainHandler>(
            reg, sink, gtnh::common::steamItemId());
        static_cast<void>(reg.create()); // dummy: occupies entity id 0
        machine = reg.create();
    }

    void registerPort(const ResourcePort& port, std::uint32_t resource_id) {
        handler->handlePortRegister(serializePortRegister(port, resource_id));
    }

    void drain(std::uint64_t request_id, PortId port_id, std::uint32_t resource_id,
               std::int32_t amount, ResourceKind kind = ResourceKind::FLUID) {
        handler->handleDrainRequest(drainPayload(request_id, port_id, kind, resource_id, amount));
    }

    const gtnh::common::ResourceTransferResponse& lastResponse() const {
        return traffic.drain_responses.back();
    }
};

const std::uint32_t kSteamId = gtnh::common::steamItemId();
const std::uint32_t kWaterId = 0xFC02; // any non-steam packed fluid id

// -- 3.2.2 + 3.2.3: FLUID drain against SteamOutputComponent, replay-safe ----

void test_drain_steam_happy_path_and_replay() {
    DrainFixture fx;
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(fx.machine);
    steam.steam_stored = 100.0;
    steam.steam_capacity = 1000.0;
    fx.registerPort(makeFluidPort(11, static_cast<std::uint64_t>(fx.machine),
                                  PortRole::SOURCE, /*rate=*/0, /*capacity=*/1000),
                    kSteamId);

    fx.traffic.clear();
    fx.drain(/*request_id=*/7, 11, kSteamId, 30);
    CHECK_EQ(fx.traffic.drain_responses.size(), std::size_t(1), "one drain response");
    CHECK_EQ(fx.lastResponse().request_id, std::uint64_t(7), "response echoes request_id");
    CHECK_EQ(fx.lastResponse().port_id, PortId(11), "response echoes port_id");
    CHECK_EQ(fx.lastResponse().resource_kind, ResourceKind::FLUID, "response echoes kind");
    CHECK_EQ(fx.lastResponse().resource_id, kSteamId, "response echoes resource_id");
    CHECK_EQ(fx.lastResponse().accepted_amount, 30, "full acceptance");
    CHECK_EQ(steam.steam_stored, 70.0, "buffer debited once");

    // Replay of the same request_id: cached response, no second debit.
    fx.drain(7, 11, kSteamId, 30);
    CHECK_EQ(fx.traffic.drain_responses.size(), std::size_t(2), "replay still answered");
    CHECK_EQ(fx.lastResponse().accepted_amount, 30, "replay returns cached response");
    CHECK_EQ(steam.steam_stored, 70.0, "replay must not debit twice");

    // A different request_id drains again.
    fx.drain(8, 11, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 10, "new request debits");
    CHECK_EQ(steam.steam_stored, 60.0, "buffer debited for the new request only");
}

void test_drain_fluid_storage_path() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId, 50, 100, 0, 40);
    fx.registerPort(makeFluidPort(12, static_cast<std::uint64_t>(fx.machine),
                                  PortRole::SOURCE, 0, 100),
                    kWaterId);

    fx.traffic.clear();
    fx.drain(1, 12, kWaterId, 20);
    CHECK_EQ(fx.lastResponse().accepted_amount, 20, "FluidStorage drained");
    CHECK_EQ(fluid.amount, 30, "buffer debited once");

    // maxOutput caps the machine-side rate even when the port has none.
    fx.drain(2, 12, kWaterId, 50);
    CHECK_EQ(fx.lastResponse().accepted_amount, 30, "short acceptance limited by maxOutput");
    CHECK_EQ(fluid.amount, 0, "buffer emptied exactly");

    // Empty buffer: zero acceptance.
    fx.drain(3, 12, kWaterId, 5);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "unavailable -> zero");
}

// -- 3.2.4: zero/short acceptance matrix -------------------------------------

void test_drain_zero_acceptance_matrix() {
    DrainFixture fx;
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(fx.machine);
    steam.steam_stored = 40.0;
    steam.steam_capacity = 100.0;
    fx.registerPort(makeFluidPort(21, static_cast<std::uint64_t>(fx.machine),
                                  PortRole::SOURCE, 0, 100),
                    kSteamId);
    const auto owner = static_cast<std::uint64_t>(fx.machine);

    // Mismatched fluid id: nothing accepted, buffer untouched.
    fx.traffic.clear();
    fx.drain(100, 21, kWaterId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "mismatched fluid -> zero");
    CHECK_EQ(steam.steam_stored, 40.0, "mismatched fluid leaves buffer unchanged");

    // Negative amount.
    fx.drain(101, 21, kSteamId, -5);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "negative amount -> zero");

    // Zero amount.
    fx.drain(102, 21, kSteamId, 0);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "zero amount -> zero");

    // Unknown port.
    fx.drain(103, 999, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "unknown port -> zero");

    // Owner 0 (no entity with that id exists in the fixture registry).
    fx.registerPort(makeFluidPort(22, 0, PortRole::SOURCE, 0, 100), kSteamId);
    fx.drain(104, 22, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "owner 0 with no entity -> zero");

    // Removed entity: owner id that was never valid.
    fx.registerPort(makeFluidPort(23, 0xDEAD, PortRole::SOURCE, 0, 100), kSteamId);
    fx.drain(105, 23, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "missing entity -> zero");

    // Kind mismatch: port is FLUID, request says HU.
    fx.drain(106, 21, kSteamId, 10, ResourceKind::HU);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "resource kind mismatch -> zero");

    // Non-source port.
    fx.registerPort(makeFluidPort(24, owner, PortRole::SINK, 0, 100), kSteamId);
    fx.drain(107, 24, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "SINK port is not drainable");

    // Entity without any fluid buffer.
    auto bare = fx.reg.create();
    fx.registerPort(makeFluidPort(25, static_cast<std::uint64_t>(bare),
                                  PortRole::SOURCE, 0, 100), kSteamId);
    fx.drain(108, 25, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "no buffer component -> zero");

    // request_id 0 is rejected and never debits.
    fx.drain(0, 21, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "request_id 0 -> zero");
    CHECK_EQ(steam.steam_stored, 40.0, "rejected matrix left buffer unchanged");

    // Short acceptance: request more than available.
    fx.drain(109, 21, kSteamId, 1000);
    CHECK_EQ(fx.lastResponse().accepted_amount, 40, "over-available request -> short acceptance");
    CHECK_EQ(steam.steam_stored, 0.0, "short acceptance drains exactly what is available");
}

void test_drain_rate_limit() {
    DrainFixture fx;
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(fx.machine);
    steam.steam_stored = 100.0;
    steam.steam_capacity = 1000.0;
    fx.registerPort(makeFluidPort(31, static_cast<std::uint64_t>(fx.machine),
                                  PortRole::SOURCE, /*rate=*/10, /*capacity=*/1000),
                    kSteamId);

    fx.traffic.clear();
    fx.drain(1, 31, kSteamId, 50);
    CHECK_EQ(fx.lastResponse().accepted_amount, 10, "port rate caps acceptance");
    CHECK_EQ(steam.steam_stored, 90.0, "only the rate-limited amount is debited");
}

// -- 2.5.2 + 2.5.4: destruction clears ports and replay state ----------------

void test_owner_removal_clears_ports_and_replay() {
    DrainFixture fx;
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(fx.machine);
    steam.steam_stored = 100.0;
    steam.steam_capacity = 1000.0;
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(41, owner, PortRole::SOURCE, 0, 1000), kSteamId);
    fx.registerPort(makeFluidPort(42, owner, PortRole::SOURCE, 0, 1000), kSteamId);

    fx.traffic.clear();
    fx.drain(9, 41, kSteamId, 25);
    CHECK_EQ(fx.lastResponse().accepted_amount, 25, "pre-removal drain accepted");
    CHECK_EQ(steam.steam_stored, 75.0, "pre-removal debit");

    fx.handler->removeOwnerPorts(owner);

    CHECK_EQ(fx.traffic.port_removes.size(), std::size_t(2),
             "ResourcePortRemove published for every port of the owner");
    bool saw_port41 = false, saw_port42 = false;
    for (const auto& key : fx.traffic.port_removes) {
        saw_port41 = saw_port41 || (key.port_id == PortId(41) && key.owner_id == owner);
        saw_port42 = saw_port42 || (key.port_id == PortId(42) && key.owner_id == owner);
    }
    CHECK(saw_port41 && saw_port42, "removes carry exact (owner, port) identity");

    // Ports are gone: a fresh request gets zero.
    fx.drain(10, 41, kSteamId, 5);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "removed port drains nothing");
    CHECK_EQ(steam.steam_stored, 75.0, "removed port does not debit");

    // Replay state for the owner was cleared: retrying request 9 is no longer
    // answered from the cache (it re-evaluates and hits the removed port).
    fx.drain(9, 41, kSteamId, 25);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0,
             "replay state cleared by owner removal");
    CHECK_EQ(steam.steam_stored, 75.0, "post-removal retry does not debit");

    // Other owners are untouched.
    auto other = fx.reg.create();
    auto& other_steam = fx.reg.emplace<simcore::SteamOutputComponent>(other);
    other_steam.steam_stored = 10.0;
    fx.registerPort(makeFluidPort(43, static_cast<std::uint64_t>(other),
                                  PortRole::SOURCE, 0, 100), kSteamId);
    fx.drain(11, 43, kSteamId, 5);
    CHECK_EQ(fx.lastResponse().accepted_amount, 5, "other owner still drainable");
}

void test_typed_port_remove_clears_port_and_replay() {
    DrainFixture fx;
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(fx.machine);
    steam.steam_stored = 50.0;
    steam.steam_capacity = 100.0;
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(51, owner, PortRole::SOURCE, 0, 100), kSteamId);

    fx.traffic.clear();
    fx.drain(1, 51, kSteamId, 20);
    CHECK_EQ(fx.lastResponse().accepted_amount, 20, "drain before typed removal");

    // Exact (owner, kind, port, epoch) removal via the wire topic.
    fx.handler->handlePortRemove(serializePortRemove(
        owner, ResourceKind::FLUID, 51, 1));

    fx.drain(2, 51, kSteamId, 20);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "port removed by exact key");
    CHECK_EQ(steam.steam_stored, 30.0, "no debit after typed removal");

    // Replay entry of the removed port was purged: request 1 re-evaluates.
    fx.drain(1, 51, kSteamId, 20);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "replay entry of removed port purged");
}

// -- 3.2.3: bounded replay cache ----------------------------------------------

void test_replay_cache_is_bounded() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId, 100000, 200000, 0, 100000);
    fx.registerPort(makeFluidPort(61, static_cast<std::uint64_t>(fx.machine),
                                  PortRole::SOURCE, 0, 200000),
                    kWaterId);

    // Fill the cache past its bound; every request debits 1 mB.
    for (std::uint64_t id = 1; id <= simcore::ResourceDrainHandler::kReplayCacheMaxEntries + 10; ++id) {
        fx.drain(id, 61, kWaterId, 1);
    }
    const std::int32_t expected_after_fill =
        100000 - static_cast<std::int32_t>(simcore::ResourceDrainHandler::kReplayCacheMaxEntries + 10);
    CHECK_EQ(fluid.amount, expected_after_fill, "each distinct request debited once");

    // The oldest entry was evicted: replaying request 1 debits again.
    fx.drain(1, 61, kWaterId, 1);
    CHECK_EQ(fx.lastResponse().accepted_amount, 1, "evicted request is treated as new");
    CHECK_EQ(fluid.amount, expected_after_fill - 1, "evicted replay re-debits");

    // A recent entry is still cached: no second debit.
    const std::uint64_t recent = simcore::ResourceDrainHandler::kReplayCacheMaxEntries + 10;
    fx.drain(recent, 61, kWaterId, 1);
    CHECK_EQ(fluid.amount, expected_after_fill - 1, "recent request still replay-cached");
}

// -- 2.6.3: epoch update matrix on the owner-side registry --------------------

void test_handler_port_epoch_update_matrix() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId,
                                                        100000, 200000, 0, 100000);
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    auto base = makeFluidPort(71, owner, PortRole::SOURCE, /*rate=*/10,
                              /*capacity=*/200000);
    base.epoch = 1;
    fx.registerPort(base, kWaterId);

    fx.traffic.clear();
    fx.drain(1, 71, kWaterId, 50);
    CHECK_EQ(fx.lastResponse().accepted_amount, 10, "epoch 1 rate applied");

    // Same-epoch re-registration is idempotent: same policy, no duplicate.
    fx.registerPort(base, kWaterId);
    fx.drain(2, 71, kWaterId, 50);
    CHECK_EQ(fx.lastResponse().accepted_amount, 10, "same epoch keeps policy");

    // A newer epoch replaces the policy.
    auto newer = base;
    newer.epoch = 2;
    newer.rate = 100;
    fx.registerPort(newer, kWaterId);
    // Probe above the old rate (10) but at the new rate (100): accepted must
    // equal the new rate, proving the policy was replaced.
    fx.drain(3, 71, kWaterId, 150);
    CHECK_EQ(fx.lastResponse().accepted_amount, 100, "newer epoch replaces policy");

    // A stale epoch is rejected: policy stays at epoch 2.
    auto stale = base;
    stale.epoch = 1;
    stale.rate = 1000;
    fx.registerPort(stale, kWaterId);
    // Same probe: if the stale rate (1000) were wrongly applied, accepted
    // would be 150, not 100.
    fx.drain(4, 71, kWaterId, 150);
    CHECK_EQ(fx.lastResponse().accepted_amount, 100, "stale epoch cannot replace policy");

    CHECK_EQ(fluid.amount, 100000 - 10 - 10 - 100 - 100,
             "each distinct request debited exactly once");
}

// -- 2.6.3 + 2.6.4: exact-key removal; wrong keys keep port AND replay --------

void test_handler_exact_removal_key_matrix() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId,
                                                        100, 200, 0, 100);
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(81, owner, PortRole::SOURCE, 0, 200), kWaterId);

    fx.traffic.clear();
    fx.drain(1, 81, kWaterId, 30);
    CHECK_EQ(fx.lastResponse().accepted_amount, 30, "drain before removal");
    CHECK_EQ(fluid.amount, 70, "debited once");

    // Wrong epoch / kind / owner: port and its replay entry survive.
    fx.handler->handlePortRemove(serializePortRemove(owner, ResourceKind::FLUID, 81, 2));
    fx.handler->handlePortRemove(serializePortRemove(owner, ResourceKind::HU, 81, 1));
    fx.handler->handlePortRemove(serializePortRemove(owner + 1, ResourceKind::FLUID, 81, 1));
    fx.drain(2, 81, kWaterId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 10, "wrong-key removals keep the port");
    fx.drain(1, 81, kWaterId, 30);
    CHECK_EQ(fx.lastResponse().accepted_amount, 30, "replay intact after wrong-key removal");
    CHECK_EQ(fluid.amount, 60, "replay did not debit again");

    // Exact (owner, kind, port, epoch) removes the port and purges its replay.
    fx.handler->handlePortRemove(serializePortRemove(owner, ResourceKind::FLUID, 81, 1));
    fx.drain(3, 81, kWaterId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "exact key removes the port");
    fx.drain(1, 81, kWaterId, 30);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "replay entry purged by exact removal");
    CHECK_EQ(fluid.amount, 60, "no debit after removal");
}

// -- 2.6.4: owner-destruction cleanup scope -----------------------------------

void test_handler_remove_owner_ports_without_ports_is_harmless() {
    DrainFixture fx;
    fx.traffic.clear();
    fx.handler->removeOwnerPorts(0xDEAD);
    CHECK(fx.traffic.port_removes.empty(), "no publishes without ports");
    CHECK(fx.traffic.drain_responses.empty(), "no responses without ports");
}

void test_handler_owner_removal_scoped_to_owner_replay() {
    DrainFixture fx;
    auto& a = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId, 100, 200, 0, 100);
    const auto owner_a = static_cast<std::uint64_t>(fx.machine);
    auto other = fx.reg.create();
    auto& b = fx.reg.emplace<simcore::FluidStorage>(other, kWaterId, 100, 200, 0, 100);
    const auto owner_b = static_cast<std::uint64_t>(other);
    fx.registerPort(makeFluidPort(91, owner_a, PortRole::SOURCE, 0, 200), kWaterId);
    fx.registerPort(makeFluidPort(92, owner_b, PortRole::SOURCE, 0, 200), kWaterId);

    fx.traffic.clear();
    fx.drain(1, 91, kWaterId, 20);
    fx.drain(2, 92, kWaterId, 20);
    CHECK_EQ(a.amount, 80, "owner A debited");
    CHECK_EQ(b.amount, 80, "owner B debited");

    fx.handler->removeOwnerPorts(owner_a);

    // Owner A: replay purged — request 1 re-evaluates against the removed port.
    fx.drain(1, 91, kWaterId, 20);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "A replay purged by owner removal");
    // Owner B: replay intact — request 2 returns the cached response.
    fx.drain(2, 92, kWaterId, 20);
    CHECK_EQ(fx.lastResponse().accepted_amount, 20, "B replay survives A removal");
    CHECK_EQ(b.amount, 80, "B replay did not debit again");
    CHECK_EQ(a.amount, 80, "A not debited after removal");
}

// -- 3.6.1: source-only conservation + combined shortfall conservation --------

void test_source_drain_conservation_exact() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId,
                                                        100, 200, 0, 100);
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(101, owner, PortRole::SOURCE, 0, 200), kWaterId);

    fx.traffic.clear();
    std::int32_t total_accepted = 0, total_debited = 0;
    const std::int32_t demands[] = {30, 50, 100};
    for (std::int32_t i = 0; i < 3; ++i) {
        const std::int32_t before = fluid.amount;
        fx.drain(static_cast<std::uint64_t>(i + 1), 101, kWaterId, demands[i]);
        const std::int32_t accepted = fx.lastResponse().accepted_amount;
        CHECK_GE(accepted, 0, "acceptance never negative");
        CHECK_EQ(before - fluid.amount, accepted, "debit == accepted per request");
        total_accepted += accepted;
        total_debited += before - fluid.amount;
    }
    CHECK_EQ(total_accepted, 100, "final short drain caps at availability");
    CHECK_EQ(total_accepted, total_debited, "conservation: accepted == debited");
    CHECK_EQ(fluid.amount, 0, "buffer drained to exactly zero");
    CHECK_GE(fluid.amount, 0, "buffer never negative");

    // Steam path: fractional storage drains its floor exactly.
    auto boiler = fx.reg.create();
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(boiler);
    steam.steam_stored = 40.5;
    steam.steam_capacity = 100.0;
    fx.registerPort(makeFluidPort(102, static_cast<std::uint64_t>(boiler),
                                  PortRole::SOURCE, 0, 200), kSteamId);
    fx.drain(10, 102, kSteamId, 100);
    CHECK_EQ(fx.lastResponse().accepted_amount, 40, "steam drains its floor amount");
    CHECK_EQ(steam.steam_stored, 0.5, "fractional remainder preserved");
}

// Combined shortfall conservation across the real owner handler (source debit)
// and the service tracker (destination credit): pipe part + source part ==
// destination credit, and replay on either leg cannot move amounts twice.
void test_combined_shortfall_conservation() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId,
                                                        200, 400, 0, 200);
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(151, owner, PortRole::SOURCE, 0, 400), kWaterId);
    gtnh::pipe_network::PipeConsumeTracker tracker(50);

    // Pipe part already served by PipeNetwork (its debit guarantee is pinned
    // by pipe_network_test); the shortfall goes to the real owner handler.
    const std::int32_t requested = 200;
    const std::int32_t pipe_accepted = 80;

    fx.traffic.clear();
    auto plan = tracker.planShortfall(
        /*drain_request_id=*/1001, /*consume_request_id=*/7, /*sink_node_id=*/30,
        kWaterId, requested, pipe_accepted, /*source_node_id=*/11, owner,
        /*source_port_id=*/151, /*now_tick=*/0);
    CHECK(plan.request_source, "shortfall emits a drain request");
    CHECK_EQ(plan.shortfall, 120, "only the shortfall is requested");

    fx.drain(plan.drain_request_id, 151, kWaterId, plan.shortfall);
    CHECK_EQ(fx.lastResponse().accepted_amount, 120, "owner accepts the full shortfall");
    CHECK_EQ(fluid.amount, 80, "source debited exactly its acceptance");

    auto applied = tracker.applyDrainResponse(plan.drain_request_id, kWaterId,
                                              fx.lastResponse().accepted_amount);
    CHECK(applied.applied, "response applies once");
    CHECK_EQ(applied.source_accepted, 120, "source part credited");
    CHECK_EQ(applied.combined_accepted, requested, "pipe part + source part == requested");
    CHECK_EQ(applied.remaining, 0, "nothing left");
    CHECK_EQ(pipe_accepted + (200 - fluid.amount), applied.combined_accepted,
             "conservation: total accepted == total debited across pipe + source");

    // Replay on both legs: no second debit, no second credit.
    fx.drain(plan.drain_request_id, 151, kWaterId, plan.shortfall);
    CHECK_EQ(fx.lastResponse().accepted_amount, 120, "drain replay returns cached response");
    CHECK_EQ(fluid.amount, 80, "drain replay does not debit twice");
    auto replay_apply = tracker.applyDrainResponse(plan.drain_request_id, kWaterId, 120);
    CHECK(!replay_apply.applied, "response replay ignored");

    // Partial source acceptance: the gap is neither debited nor credited.
    auto plan2 = tracker.planShortfall(1002, 8, 30, kWaterId, 100, 0, 11, owner, 151, 0);
    CHECK_EQ(plan2.shortfall, 100, "full demand is the shortfall");
    fx.drain(plan2.drain_request_id, 151, kWaterId, plan2.shortfall);
    CHECK_EQ(fx.lastResponse().accepted_amount, 80, "owner short-accepts at availability");
    CHECK_EQ(fluid.amount, 0, "source debited only what it has");
    auto applied2 = tracker.applyDrainResponse(plan2.drain_request_id, kWaterId,
                                               fx.lastResponse().accepted_amount);
    CHECK_EQ(applied2.combined_accepted, 80, "combined == pipe + actual source part");
    CHECK_EQ(applied2.remaining, 20, "unserved gap reported exactly");
    CHECK_GE(fluid.amount, 0, "source never over-debited");
}

// -- 3.6.2: partial acceptance bounds on the owner side -----------------------

void test_drain_partial_acceptance_bounds() {
    DrainFixture fx;
    // maxOutput caps every debit; repeated partial drains sum exactly.
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId,
                                                        25, 200, 0, 10);
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(111, owner, PortRole::SOURCE, 0, 200), kWaterId);

    fx.traffic.clear();
    std::int32_t total = 0;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        fx.drain(id, 111, kWaterId, 50);
        const std::int32_t accepted = fx.lastResponse().accepted_amount;
        CHECK(accepted <= 10, "each drain bounded by maxOutput");
        total += accepted;
    }
    CHECK_EQ(total, 25, "three capped drains empty the buffer exactly");
    CHECK_EQ(fluid.amount, 0, "buffer exactly empty");
    fx.drain(4, 111, kWaterId, 50);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "empty buffer accepts zero");
    CHECK_EQ(fluid.amount, 0, "zero acceptance keeps the buffer at zero");
}

// -- 3.6.3: replay determinism for unknown ports and request id zero ----------

void test_handler_unknown_port_response_cached() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId,
                                                        100, 200, 0, 100);
    const auto owner = static_cast<std::uint64_t>(fx.machine);

    fx.traffic.clear();
    fx.drain(5, 777, kWaterId, 40);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "unknown port -> zero");
    fx.drain(5, 777, kWaterId, 40);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "retry returns the cached zero");
    CHECK_EQ(fluid.amount, 100, "no debit for unknown port");

    // The port appears later; the cached zero still wins for request 5
    // (a redelivered request must not re-execute).
    fx.registerPort(makeFluidPort(777, owner, PortRole::SOURCE, 0, 200), kWaterId);
    fx.drain(5, 777, kWaterId, 40);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "cached response trumps late port");
    CHECK_EQ(fluid.amount, 100, "replayed unknown-port request never debits");

    // A fresh request id uses the now-registered port.
    fx.drain(6, 777, kWaterId, 40);
    CHECK_EQ(fx.lastResponse().accepted_amount, 40, "new request id drains");
    CHECK_EQ(fluid.amount, 60, "fresh request debits once");
}

void test_handler_request_id_zero_never_debits() {
    DrainFixture fx;
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(fx.machine, kWaterId,
                                                        100, 200, 0, 100);
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(121, owner, PortRole::SOURCE, 0, 200), kWaterId);

    fx.traffic.clear();
    fx.drain(0, 121, kWaterId, 30);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "request_id 0 rejected");
    CHECK_EQ(fx.lastResponse().request_id, std::uint64_t(0), "zero response echoes the id");
    fx.drain(0, 121, kWaterId, 30);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "second zero-id request also rejected");
    CHECK_EQ(fluid.amount, 100,
             "zero-id requests never debit (redelivery would double-debit)");
}

// -- 3.6.4: mixed fluids and capacity boundaries on the owner side ------------

void test_drain_mixed_fluids_excluded() {
    DrainFixture fx;
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(fx.machine);
    steam.steam_stored = 50.0;
    steam.steam_capacity = 100.0;
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(131, owner, PortRole::SOURCE, 0, 100), kSteamId);

    fx.traffic.clear();
    fx.drain(1, 131, kWaterId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "water request excluded from steam buffer");
    CHECK_EQ(steam.steam_stored, 50.0, "mismatched request leaves steam unchanged");

    auto tank = fx.reg.create();
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(tank, kWaterId, 60, 100, 0, 60);
    fx.registerPort(makeFluidPort(132, static_cast<std::uint64_t>(tank),
                                  PortRole::SOURCE, 0, 100), kWaterId);
    fx.drain(2, 132, kSteamId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "steam request excluded from water tank");
    CHECK_EQ(fluid.amount, 60, "mismatched request leaves the tank unchanged");

    auto empty = fx.reg.create();
    auto& ef = fx.reg.emplace<simcore::FluidStorage>(empty, 0, 0, 100, 0, 60);
    fx.registerPort(makeFluidPort(133, static_cast<std::uint64_t>(empty),
                                  PortRole::SOURCE, 0, 100), kWaterId);
    fx.drain(3, 133, kWaterId, 10);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "empty tank excludes everything");
    CHECK_EQ(ef.amount, 0, "empty tank unchanged");
}

void test_drain_capacity_boundaries_exact_fill() {
    DrainFixture fx;
    auto& steam = fx.reg.emplace<simcore::SteamOutputComponent>(fx.machine);
    steam.steam_stored = 100.0;
    steam.steam_capacity = 100.0;
    const auto owner = static_cast<std::uint64_t>(fx.machine);
    fx.registerPort(makeFluidPort(141, owner, PortRole::SOURCE, 0, 100), kSteamId);

    fx.traffic.clear();
    fx.drain(1, 141, kSteamId, 1000);
    CHECK_EQ(fx.lastResponse().accepted_amount, 100, "over-available demand short-fills");
    CHECK_EQ(steam.steam_stored, 0.0, "buffer drained to exactly zero");
    fx.drain(2, 141, kSteamId, 1);
    CHECK_EQ(fx.lastResponse().accepted_amount, 0, "empty buffer accepts zero");

    auto tank = fx.reg.create();
    auto& fluid = fx.reg.emplace<simcore::FluidStorage>(tank, kWaterId, 40, 100, 0, 100);
    fx.registerPort(makeFluidPort(142, static_cast<std::uint64_t>(tank),
                                  PortRole::SOURCE, 0, 100), kWaterId);
    fx.drain(3, 142, kWaterId, 40);
    CHECK_EQ(fx.lastResponse().accepted_amount, 40, "exact-fill request fully accepted");
    CHECK_EQ(fluid.amount, 0, "tank exactly empty");
    CHECK(fluid.isEmpty(), "isEmpty boundary holds");
}

} // namespace

void test_resource_drain() {
    printf("  TEST: drain_steam_happy_path_and_replay\n");
    test_drain_steam_happy_path_and_replay();
    printf("  TEST: drain_fluid_storage_path\n");
    test_drain_fluid_storage_path();
    printf("  TEST: drain_zero_acceptance_matrix\n");
    test_drain_zero_acceptance_matrix();
    printf("  TEST: drain_rate_limit\n");
    test_drain_rate_limit();
    printf("  TEST: owner_removal_clears_ports_and_replay\n");
    test_owner_removal_clears_ports_and_replay();
    printf("  TEST: typed_port_remove_clears_port_and_replay\n");
    test_typed_port_remove_clears_port_and_replay();
    printf("  TEST: replay_cache_is_bounded\n");
    test_replay_cache_is_bounded();
    printf("  TEST: handler_port_epoch_update_matrix\n");
    test_handler_port_epoch_update_matrix();
    printf("  TEST: handler_exact_removal_key_matrix\n");
    test_handler_exact_removal_key_matrix();
    printf("  TEST: handler_remove_owner_ports_without_ports_is_harmless\n");
    test_handler_remove_owner_ports_without_ports_is_harmless();
    printf("  TEST: handler_owner_removal_scoped_to_owner_replay\n");
    test_handler_owner_removal_scoped_to_owner_replay();
    printf("  TEST: source_drain_conservation_exact\n");
    test_source_drain_conservation_exact();
    printf("  TEST: combined_shortfall_conservation\n");
    test_combined_shortfall_conservation();
    printf("  TEST: drain_partial_acceptance_bounds\n");
    test_drain_partial_acceptance_bounds();
    printf("  TEST: handler_unknown_port_response_cached\n");
    test_handler_unknown_port_response_cached();
    printf("  TEST: handler_request_id_zero_never_debits\n");
    test_handler_request_id_zero_never_debits();
    printf("  TEST: drain_mixed_fluids_excluded\n");
    test_drain_mixed_fluids_excluded();
    printf("  TEST: drain_capacity_boundaries_exact_fill\n");
    test_drain_capacity_boundaries_exact_fill();
}
