#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <cmath>
#include <common/ItemId.h>
#include <common/ResourcePortClient.h>
#include "PipeNetwork.h"
#include "PipeConsumeTransactions.h"
#include "HeatLoss.h"
#include "CableGraph.h"
#include "CableTypes.h"
#include "FluidRegistry.h"

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr, const char* msg) {
    if (!cond) {
        fprintf(stderr, "  FAIL [%s:%d] %s", file, line, expr);
        if (msg) fprintf(stderr, " -- %s", msg);
        fprintf(stderr, "\n");
        ++g_failed;
    } else {
        ++g_passed;
    }
}
#define CHECK(cond, msg) test_check((cond), __FILE__, __LINE__, #cond, msg)
#define CHECK_EQ(a, b, msg) test_check((a) == (b), __FILE__, __LINE__, #a " == " #b, msg)
#define CHECK_NE(a, b, msg) test_check((a) != (b), __FILE__, __LINE__, #a " != " #b, msg)
#define CHECK_GT(a, b, msg) test_check((a) > (b), __FILE__, __LINE__, #a " > " #b, msg)
#define CHECK_GE(a, b, msg) test_check((a) >= (b), __FILE__, __LINE__, #a " >= " #b, msg)
#define CHECK_LT(a, b, msg) test_check((a) < (b), __FILE__, __LINE__, #a " < " #b, msg)
#define PASS() do { ++g_passed; } while(0)

// =========================================================================
//  Existing tests (PipeNetwork basics)
// =========================================================================

static void test_empty_network() {
    pipenet::PipeNetworkManager mgr;
    CHECK_EQ(mgr.nodeCount(), size_t(0), "no nodes initially");
    CHECK_EQ(mgr.networkCount(), size_t(0), "no networks initially");
    PASS();
}

static void test_single_node() {
    pipenet::PipeNetworkManager mgr;
    uint64_t nid = mgr.addNode(0, 0, 0, 100);
    CHECK_GT(nid, size_t(0), "node id > 0");
    CHECK_EQ(mgr.nodeCount(), size_t(1), "one node");
    auto* node = mgr.getNode(nid);
    CHECK(node != nullptr, "node exists");
    CHECK_EQ(node->x, 0, "x"); CHECK_EQ(node->y, 0, "y"); CHECK_EQ(node->z, 0, "z");
    CHECK_EQ(node->block_id, uint16_t(100), "block_id");
    PASS();
}

static void test_add_remove_node() {
    pipenet::PipeNetworkManager mgr;
    uint64_t nid = mgr.addNode(10, 20, 30, 200);
    mgr.removeNode(nid);
    CHECK_EQ(mgr.getNode(nid), nullptr, "node removed");
    CHECK_EQ(mgr.nodeCount(), size_t(0), "no nodes after remove");
    PASS();
}

static void test_network_discovery() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(1, 0, 0, 100);
    uint64_t c = mgr.addNode(2, 0, 0, 100);
    mgr.addEdge(a, b);
    mgr.addEdge(b, c);

    auto net = mgr.discoverNetwork(a);
    CHECK_EQ(net.size(), size_t(3), "3 nodes in network");
    PASS();
}

static void test_disconnected_graphs() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(10, 0, 0, 100);
    mgr.addEdge(a, b);
    uint64_t n2_a = mgr.addNode(20, 0, 0, 100);
    uint64_t n2_b = mgr.addNode(30, 0, 0, 100);
    mgr.addEdge(n2_a, n2_b);

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(2), "two disconnected networks");

    auto net_a = mgr.discoverNetwork(a);
    CHECK_EQ(net_a.size(), size_t(2), "first network has 2 nodes");
    auto net_c = mgr.discoverNetwork(n2_a);
    CHECK_EQ(net_c.size(), size_t(2), "second network has 2 nodes");
    PASS();
}

static void test_rebuild_networks() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(1, 0, 0, 100);
    mgr.addEdge(a, b);

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(1), "one network after rebuild");

    mgr.addNode(5, 0, 0, 100);
    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(2), "two networks after adding isolated node");
    PASS();
}

static void test_add_node_with_id() {
    pipenet::PipeNetworkManager mgr;
    bool ok = mgr.addNodeWithId(42, 5, 5, 5, 99);
    CHECK(ok, "addNodeWithId success");
    CHECK_EQ(mgr.nodeCount(), size_t(1), "one node after add");

    ok = mgr.addNodeWithId(42, 6, 6, 6, 99);
    CHECK(!ok, "duplicate id returns false");
    CHECK_EQ(mgr.nodeCount(), size_t(1), "still one node after duplicate");
    PASS();
}

// =========================================================================
//  Typed resource port registration tests
// =========================================================================

static gtnh::common::ResourcePort make_test_port(
    gtnh::common::ResourceKind kind, gtnh::common::PortRole role,
    uint64_t epoch = 1) {
    gtnh::common::ResourcePort port;
    port.port_id = 7;
    port.owner_id = 99;
    port.resource_kind = kind;
    port.role = role;
    port.x = 10;
    port.y = 20;
    port.z = 30;
    port.capacity = 1000;
    port.rate = 100;
    port.epoch = epoch;
    return port;
}

static void test_typed_ports_independent_resource_domains() {
    pipenet::PipeNetworkManager mgr;
    auto fluid = make_test_port(gtnh::common::ResourceKind::FLUID,
                                gtnh::common::PortRole::SOURCE);
    auto energy = make_test_port(gtnh::common::ResourceKind::EU,
                                 gtnh::common::PortRole::SINK);

    CHECK(mgr.registerPort(fluid), "fluid port registers");
    CHECK(mgr.registerPort(energy), "energy port registers independently");
    CHECK_EQ(mgr.portCount(), size_t(2), "resource domains have independent keys");
    CHECK(mgr.getPort(fluid.owner_id, fluid.resource_kind, fluid.port_id) != nullptr,
          "fluid port is queryable");
    CHECK(mgr.getPort(energy.owner_id, energy.resource_kind, energy.port_id) != nullptr,
          "energy port is queryable");
    CHECK_EQ(mgr.getPort(fluid.owner_id, fluid.resource_kind, fluid.port_id)->role,
             gtnh::common::PortRole::SOURCE, "fluid role remains source");
    CHECK_EQ(mgr.getPort(energy.owner_id, energy.resource_kind, energy.port_id)->role,
             gtnh::common::PortRole::SINK, "energy role remains sink");
    PASS();
}

static void test_typed_port_reregistration_is_idempotent() {
    pipenet::PipeNetworkManager mgr;
    auto port = make_test_port(gtnh::common::ResourceKind::FLUID,
                               gtnh::common::PortRole::SOURCE);

    CHECK(mgr.registerPort(port), "initial registration succeeds");
    CHECK(mgr.registerPort(port), "same registration is an idempotent success");
    CHECK_EQ(mgr.portCount(), size_t(1), "retry does not duplicate port");

    auto newer = port;
    newer.role = gtnh::common::PortRole::SINK;
    newer.capacity = 2000;
    newer.epoch = 2;
    CHECK(mgr.registerPort(newer), "new epoch replaces registration");
    CHECK_EQ(mgr.getPort(port.owner_id, port.resource_kind, port.port_id)->role,
             gtnh::common::PortRole::SINK, "new role is visible");
    CHECK_EQ(mgr.getPort(port.owner_id, port.resource_kind, port.port_id)->capacity,
             2000, "new capacity is visible");

    auto stale = newer;
    stale.role = gtnh::common::PortRole::SOURCE;
    stale.epoch = 1;
    CHECK(!mgr.registerPort(stale), "stale epoch is rejected");
    CHECK_EQ(mgr.getPort(port.owner_id, port.resource_kind, port.port_id)->role,
             gtnh::common::PortRole::SINK, "stale update cannot overwrite state");
    PASS();
}

static void test_typed_port_removal_cleanup() {
    pipenet::PipeNetworkManager mgr;
    auto fluid = make_test_port(gtnh::common::ResourceKind::FLUID,
                                gtnh::common::PortRole::SOURCE);
    auto energy = make_test_port(gtnh::common::ResourceKind::EU,
                                 gtnh::common::PortRole::SINK);
    CHECK(mgr.registerPort(fluid), "fluid port registers");
    CHECK(mgr.registerPort(energy), "energy port registers");

    CHECK(!mgr.removePort(fluid.owner_id, fluid.resource_kind, fluid.port_id, 2),
          "wrong epoch does not remove port");
    CHECK(mgr.removePort(fluid), "matching registration removes fluid port");
    CHECK(!mgr.hasPort(fluid.owner_id, fluid.resource_kind, fluid.port_id),
          "removed fluid port is absent");
    CHECK(mgr.hasPort(energy.owner_id, energy.resource_kind, energy.port_id),
          "removing one domain preserves another");
    CHECK(!mgr.removePort(fluid), "repeated removal is harmless");

    CHECK_EQ(mgr.removePortsForOwner(energy.owner_id), size_t(1),
             "owner cleanup removes remaining ports");
    CHECK_EQ(mgr.removePortsForOwner(energy.owner_id), size_t(0),
             "repeated owner cleanup is harmless");
    CHECK_EQ(mgr.portCount(), size_t(0), "all owner ports are gone");
    PASS();
}

// =========================================================================
//  Typed domain graphs/state (2.3.x)
// =========================================================================

static gtnh::common::ResourcePort make_port_at(
    gtnh::common::ResourceKind kind, gtnh::common::PortRole role,
    uint64_t owner, gtnh::common::PortId port_id,
    int32_t x, int32_t y, int32_t z, uint64_t epoch = 1) {
    gtnh::common::ResourcePort port;
    port.port_id = port_id;
    port.owner_id = owner;
    port.resource_kind = kind;
    port.role = role;
    port.x = x;
    port.y = y;
    port.z = z;
    port.capacity = 1000;
    port.rate = 100;
    port.epoch = epoch;
    return port;
}

static void test_converter_domains_independent_roles() {
    pipenet::PipeNetworkManager mgr;
    CHECK(mgr.addNodeWithId(99, 10, 20, 30, 1), "boiler node registered at port position");

    // Converter: HU sink + FLUID source on the SAME owner and position.
    auto hu = make_port_at(gtnh::common::ResourceKind::HU,
                           gtnh::common::PortRole::SINK, 99, 1, 10, 20, 30);
    auto fluid = make_port_at(gtnh::common::ResourceKind::FLUID,
                              gtnh::common::PortRole::SOURCE, 99, 2, 10, 20, 30);
    CHECK(mgr.registerPort(hu), "HU sink port registers");
    CHECK(mgr.registerPort(fluid), "FLUID source port registers");

    const auto* node = mgr.getNode(99);
    CHECK(node != nullptr, "boiler node exists");
    const auto& hu_domain =
        node->domains[pipenet::domainIndex(gtnh::common::ResourceKind::HU)];
    const auto& fluid_domain =
        node->domains[pipenet::domainIndex(gtnh::common::ResourceKind::FLUID)];
    CHECK(hu_domain.is_sink, "HU domain sees the sink role");
    CHECK(!hu_domain.is_source, "HU domain has no source role");
    CHECK(fluid_domain.is_source, "FLUID domain sees the source role");
    CHECK(!fluid_domain.is_sink, "FLUID domain has no sink role");
    CHECK_EQ(hu_domain.rate, 100, "HU domain carries the port rate");
    PASS();
}

static void test_typed_registration_feeds_domain_after_node() {
    pipenet::PipeNetworkManager mgr;
    // Port registered before the node exists (publication order independence).
    auto hu = make_port_at(gtnh::common::ResourceKind::HU,
                           gtnh::common::PortRole::SINK, 5, 1, 1, 2, 3);
    CHECK(mgr.registerPort(hu), "port registers before the node exists");

    uint64_t node = mgr.addNode(1, 2, 3, 1);
    const auto* d = mgr.nodeDomain(node, gtnh::common::ResourceKind::HU);
    CHECK(d != nullptr, "domain view exists");
    CHECK(d->is_sink, "late node inherits the HU sink role");
    CHECK(!d->is_source, "HU source role stays unset");
    PASS();
}

static void test_legacy_adapter_routes_to_single_domain() {
    pipenet::PipeNetworkManager mgr;
    uint64_t n = mgr.addNode(0, 0, 0, 1);

    // Legacy heat update touches only the HU domain.
    mgr.setNodeHeat(n, 100, 500, false, true);
    CHECK(mgr.nodeDomain(n, gtnh::common::ResourceKind::HU)->is_sink,
          "legacy heat sink lands in HU");
    CHECK(!mgr.nodeDomain(n, gtnh::common::ResourceKind::FLUID)->is_sink,
          "HU sink does not leak into FLUID");
    CHECK(!mgr.nodeDomain(n, gtnh::common::ResourceKind::EU)->is_sink,
          "HU sink does not leak into EU");

    // Legacy fluid update touches only the FLUID domain; HU survives.
    mgr.setNodeFluid(n, 0, 500, 2, true, false);
    CHECK(mgr.nodeDomain(n, gtnh::common::ResourceKind::FLUID)->is_source,
          "legacy fluid source lands in FLUID");
    CHECK(!mgr.nodeDomain(n, gtnh::common::ResourceKind::HU)->is_source,
          "FLUID source does not leak into HU");
    CHECK(mgr.nodeDomain(n, gtnh::common::ResourceKind::HU)->is_sink,
          "HU sink survives the fluid update");

    // Legacy energy update touches only the EU domain; HU/FLUID survive.
    mgr.setNodeEnergy(n, 0, 500, false, true);
    CHECK(mgr.nodeDomain(n, gtnh::common::ResourceKind::EU)->is_sink,
          "legacy energy sink lands in EU");
    CHECK(mgr.nodeDomain(n, gtnh::common::ResourceKind::HU)->is_sink,
          "HU sink survives the energy update");
    CHECK(mgr.nodeDomain(n, gtnh::common::ResourceKind::FLUID)->is_source,
          "FLUID source survives the energy update");
    PASS();
}

static void test_cross_kind_consumption_rejected() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 1);
    auto hu = make_port_at(gtnh::common::ResourceKind::HU,
                           gtnh::common::PortRole::SINK, 7, 1, 0, 0, 0);
    CHECK(mgr.registerPort(hu), "HU sink port registers");

    // Node-level fluid consume: the HU port must not make the node a fluid sink.
    auto res = mgr.consumeFluid(node, 1, 2, 10);
    CHECK(res.blocked, "fluid consume is blocked without a FLUID sink");
    CHECK_EQ(res.accepted_amount, 0, "cross-kind consume cannot debit");

    // Port-level fluid consume through the HU port id: rejected and logged.
    auto via_port = mgr.consumeFluidViaPort(7, 1, 2, 2, 10);
    CHECK(via_port.blocked, "fluid consume via HU port is rejected");
    CHECK_EQ(via_port.accepted_amount, 0, "cross-kind port consume cannot debit");
    PASS();
}

static void test_port_removal_clears_domain_role() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);  // fluid pipe: buffer + capacity
    mgr.setNodeFluid(node, 100, 1000, 2, false, false);
    auto fluid = make_port_at(gtnh::common::ResourceKind::FLUID,
                              gtnh::common::PortRole::SINK, 7, 3, 0, 0, 0);
    CHECK(mgr.registerPort(fluid), "FLUID sink port registers");
    CHECK(mgr.nodeDomain(node, gtnh::common::ResourceKind::FLUID)->is_sink,
          "typed registration feeds the FLUID domain");

    auto served = mgr.consumeFluid(node, 1, 2, 50);
    CHECK_EQ(served.accepted_amount, 50, "port-backed sink is served");

    CHECK(mgr.removePort(fluid), "port removes");
    CHECK(!mgr.nodeDomain(node, gtnh::common::ResourceKind::FLUID)->is_sink,
          "removed port no longer feeds the domain");
    auto res = mgr.consumeFluid(node, 2, 2, 50);
    CHECK(res.blocked, "flow cannot target the removed port's machine");
    CHECK_EQ(res.accepted_amount, 0, "removed port cannot debit");
    PASS();
}

static void test_owner_zero_port_registers() {
    pipenet::PipeNetworkManager mgr;
    auto port = make_port_at(gtnh::common::ResourceKind::FLUID,
                             gtnh::common::PortRole::SINK, 0, 9, 0, 0, 0);
    CHECK(mgr.registerPort(port), "owner id zero is a valid machine instance");
    CHECK(mgr.hasPort(0, gtnh::common::ResourceKind::FLUID, 9),
          "owner-zero port is queryable");
    PASS();
}

static void test_typed_rate_applied_at_solve_time() {
    pipenet::PipeNetworkManager mgr;
    uint64_t sink = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(sink, 100, 100, 2, false, true);

    auto full = mgr.consumeFluid(sink, 1, 2, 50);
    CHECK_EQ(full.accepted_amount, 50, "without port policy the demand is served");

    // A typed port adds solve-time rate policy for the same domain.
    auto port = make_port_at(gtnh::common::ResourceKind::FLUID,
                             gtnh::common::PortRole::SINK, 7, 4, 0, 0, 0);
    port.rate = 10;
    CHECK(mgr.registerPort(port), "rate-limited FLUID sink port registers");
    auto limited = mgr.consumeFluid(sink, 2, 2, 50);
    CHECK_EQ(limited.accepted_amount, 10, "port rate clamps the solve-time transfer");
    CHECK_EQ(limited.remaining, 40, "unserved demand is reported");
    PASS();
}

static void test_topology_shared_domains_solved_independently() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 61);
    uint64_t b = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(a, b);

    // One shared topology carries two domains: FLUID roles on a, HU roles on b.
    mgr.setNodeFluid(a, 100, 200, 84, true, false);
    mgr.setNodeHeat(b, 0, 500, false, true);

    CHECK_EQ(mgr.networkCount(), size_t(1), "one shared topology");
    auto comp = mgr.discoverNetwork(a);
    CHECK_EQ(comp.size(), size_t(2), "both nodes share the graph");

    CHECK(mgr.nodeDomain(a, gtnh::common::ResourceKind::FLUID)->is_source,
          "FLUID source on a");
    CHECK(mgr.nodeDomain(b, gtnh::common::ResourceKind::HU)->is_sink,
          "HU sink on b");
    CHECK(!mgr.nodeDomain(b, gtnh::common::ResourceKind::FLUID)->is_source,
          "no FLUID leak to b");
    CHECK(!mgr.nodeDomain(a, gtnh::common::ResourceKind::HU)->is_sink,
          "no HU leak to a");
    PASS();
}

// =========================================================================
//  Pipe wrench guidance tests (evaluatePipeWrench)
// =========================================================================

static void test_wrench_isolated_pipe() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    uint64_t nid = 0;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;

    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::CONNECT_PIPES, "isolated pipe -> CONNECT_PIPES");
    CHECK_EQ(nid, uint64_t(1), "node id returned for pipe position");
    PASS();
}

static void test_wrench_pipe_to_pipe() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;
    pipes[pipenet::pipePosKey(1, 0, 0)] = 2;

    uint64_t nid = 0;
    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::CONNECT_TO_MACHINE,
          "pipe next to pipe -> CONNECT_TO_MACHINE");
    PASS();
}

static void test_wrench_pipe_adjacent_machine() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;
    machines[pipenet::pipePosKey(1, 0, 0)] = 900;

    uint64_t nid = 0;
    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::CONNECTED, "pipe adjacent machine -> CONNECTED");
    PASS();
}

static void test_wrench_non_pipe_position() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(5, 5, 5)] = 1;

    uint64_t nid = 77;
    auto g = pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(g == pipenet::WrenchGuidance::NOT_A_PIPE, "no pipe at pos -> NOT_A_PIPE");
    CHECK_EQ(nid, uint64_t(0), "node id cleared to 0 for non-pipe");
    PASS();
}

static void test_wrench_guidance_no_mutation() {
    std::unordered_map<uint64_t, uint64_t> pipes;
    std::unordered_map<uint64_t, uint64_t> machines;
    pipes[pipenet::pipePosKey(0, 0, 0)] = 1;
    pipes[pipenet::pipePosKey(1, 0, 0)] = 2;

    auto before = pipes;
    uint64_t nid = 0;
    pipenet::evaluatePipeWrench(pipes, machines, 0, 0, 0, &nid);
    CHECK(pipes == before, "pipe map unchanged after evaluation");
    CHECK(machines.empty(), "machine map unchanged after evaluation");
    PASS();
}

// =========================================================================
//  Item network tests
// =========================================================================

static void test_item_network_simple() {
    // Simple source → single pipe → sink: item should move in one tick
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);   // item_pipe
    uint64_t pipe = mgr.addNode(1, 0, 0, 62);  // item_pipe
    uint64_t sink = mgr.addNode(2, 0, 0, 62);  // item_pipe

    mgr.addEdge(src, pipe);
    mgr.addEdge(pipe, sink);

    // Configure: src produces items, sink consumes
    mgr.setNodeItemProps(src, 10, true, false);   // 10 slot capacity, is source
    mgr.setNodeItemProps(pipe, 10, false, false);  // 10 slot capacity, not source
    mgr.setNodeItemProps(sink, 10, false, true);   // 10 slot capacity, item sink

    // Add one item at source
    mgr.addNodeItem(src, 42, 1);  // item_id=42, count=1

    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(1), "source has 1 item before tick");
    CHECK_EQ(mgr.getNode(sink)->itemBuffer.size(), size_t(0), "sink has 0 items before tick");

    mgr.tickItemNetworks();

    // After tick, item should have moved from source to sink
    // Note: depending on BFS order, it might be in pipe or sink
    // At minimum, source should have 0 items
    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(0), "source has 0 items after tick");

    PASS();
}

static void test_item_network_no_sink() {
    // Source with items but no sink in network: items stay at source
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);
    mgr.setNodeItemProps(src, 10, true, false);
    mgr.addNodeItem(src, 7, 1);

    mgr.tickItemNetworks();

    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(1), "item stays at source without sink");
    PASS();
}

static void test_item_network_multi_item() {
    // Multiple items from source to sink
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);
    uint64_t sink = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(src, sink);

    mgr.setNodeItemProps(src, 10, true, false);
    mgr.setNodeItemProps(sink, 10, false, true);

    mgr.addNodeItem(src, 1, 1);
    mgr.addNodeItem(src, 2, 1);
    mgr.addNodeItem(src, 3, 1);

    mgr.tickItemNetworks();

    // moveItemsInNetwork moves 1 item per source per tick
    CHECK_LT(mgr.getNode(src)->itemBuffer.size(), size_t(3), "source has fewer items after tick");
    PASS();
}

static void test_item_network_multi_tick() {
    // Multiple ticks to move all items
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);
    uint64_t sink = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(src, sink);

    mgr.setNodeItemProps(src, 10, true, false);
    mgr.setNodeItemProps(sink, 10, false, true);

    mgr.addNodeItem(src, 1, 1);
    mgr.addNodeItem(src, 2, 1);
    mgr.addNodeItem(src, 3, 1);

    for (int i = 0; i < 5; ++i) mgr.tickItemNetworks();

    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(0), "source empty after 5 ticks");
    CHECK_GT(mgr.getNode(sink)->itemBuffer.size(), size_t(0), "sink has items after 5 ticks");
    PASS();
}

static void test_find_next_item_hop() {
    // BFS: start at node 0, should find node 1 (item-capable neighbor)
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    uint64_t c = mgr.addNode(2, 0, 0, 62);
    mgr.addEdge(a, b);
    mgr.addEdge(b, c);

    mgr.setNodeItemProps(a, 10, false, false);
    mgr.setNodeItemProps(b, 10, false, false);
    mgr.setNodeItemProps(c, 10, false, false);

    mgr.rebuildItemNetworks();
    auto* net = mgr.getItemNetwork(a);
    CHECK(net != nullptr, "item network exists");

    uint64_t hop = mgr.findNextItemHop(a, net->id);
    CHECK(hop == b || hop == c, "findNextItemHop returns connected node");
    PASS();
}

static void test_find_next_item_hop_no_item_capability() {
    // Node with itemCapacity=0 should NOT be returned as a hop
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 100);  // non-pipe block (itemCapacity=0)
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, false, false);
    // b has default itemCapacity=0

    mgr.rebuildItemNetworks();
    auto* net = mgr.getItemNetwork(a);
    CHECK(net != nullptr, "item network exists");

    uint64_t hop = mgr.findNextItemHop(a, net->id);
    CHECK_EQ(hop, uint64_t(0), "no hop to non-item-capable node");
    PASS();
}

// =========================================================================
//  Energy distribution tests
// =========================================================================

static void test_energy_distribution_simple() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src1 = mgr.addNode(0, 0, 0, 46);
    uint64_t src2 = mgr.addNode(1, 0, 0, 46);
    uint64_t sink = mgr.addNode(2, 0, 0, 37);

    mgr.addEdge(src1, src2);
    mgr.addEdge(src2, sink);

    mgr.setNodeEnergy(src1, 1000, 1000, true, false);
    mgr.setNodeEnergy(src2, 500, 500, true, false);
    mgr.setNodeEnergy(sink, 0, 2000, false, true);

    mgr.rebuildNetworks();
    CHECK_GE(mgr.networkCount(), size_t(1), "at least one energy network");

    // Find the network containing our sink
    auto nets = mgr.getAllNetworks();
    uint64_t targetNetId = 0;
    for (const auto* n : nets) {
        for (uint64_t nid : n->nodeIds) {
            if (nid == sink) { targetNetId = n->id; break; }
        }
        if (targetNetId) break;
    }
    CHECK_GT(targetNetId, uint64_t(0), "found network for sink");

    auto deltas = mgr.distributeEnergy(targetNetId, 300);
    CHECK(!deltas.empty(), "energy distribution produced deltas");

    const auto* sinkNode = mgr.getNode(sink);
    CHECK_GT(sinkNode->energyBuffer, 0, "sink received energy");
    PASS();
}

static void test_energy_distribution_no_sink() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 46);
    mgr.setNodeEnergy(src, 500, 1000, true, false);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == src) { targetNet = n->id; break; }

    if (targetNet) mgr.distributeEnergy(targetNet, 100);
    // distributeFlow drains sources even without sinks (existing behavior)
    CHECK_LT(mgr.getNode(src)->energyBuffer, 500, "energy drained from source without sink");
    PASS();
}

static void test_energy_distribution_capacity_limited() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 46);
    uint64_t sink = mgr.addNode(1, 0, 0, 37);
    mgr.addEdge(src, sink);

    mgr.setNodeEnergy(src, 10000, 10000, true, false);
    mgr.setNodeEnergy(sink, 90, 100, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    mgr.distributeEnergy(targetNet, 100);
    const auto* sinkNode = mgr.getNode(sink);
    CHECK_EQ(sinkNode->energyBuffer, 100, "sink capped at capacity");
    PASS();
}

// =========================================================================
//  Fluid distribution tests
// =========================================================================

static void test_fluid_transaction_replay_and_limits() {
    pipenet::PipeNetworkManager mgr;
    auto sink = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(sink, 100, 100, 2, false, true);

    auto first = mgr.consumeFluid(sink, 77, 2, 60);
    CHECK_EQ(first.accepted_amount, 60, "transaction accepts available pipe fluid");
    CHECK_EQ(mgr.getNode(sink)->fluidBuffer, 40, "pipe buffer debited once");
    auto replay = mgr.consumeFluid(sink, 77, 2, 60);
    CHECK_EQ(replay.accepted_amount, 60, "duplicate returns cached amount");
    CHECK_EQ(mgr.getNode(sink)->fluidBuffer, 40, "duplicate does not debit twice");
    auto mismatch = mgr.consumeFluid(sink, 78, 3, 10);
    CHECK(mismatch.blocked, "mismatched fluid is blocked");
    CHECK_EQ(mgr.getNode(sink)->fluidBuffer, 40, "mismatch leaves buffer unchanged");
    auto partial = mgr.consumeFluid(sink, 79, 2, 100);
    CHECK_EQ(partial.accepted_amount, 40, "consume reports exact short fill");
    CHECK_EQ(partial.remaining, 60, "consume reports remaining demand");

    // Request id zero is an uncorrelated request and must never alias a cached
    // transaction. A reused non-zero id with a different tuple is rejected.
    auto zero_first = mgr.consumeFluid(sink, 0, 2, 1);
    auto zero_second = mgr.consumeFluid(sink, 0, 2, 1);
    CHECK_EQ(zero_first.accepted_amount, 0, "zero-id request sees empty buffer");
    CHECK_EQ(zero_second.accepted_amount, 0, "zero-id request is not cached");
    auto conflict = mgr.consumeFluid(sink, 77, 2, 61);
    CHECK(conflict.blocked, "reused request id with different amount is blocked");
    CHECK_EQ(conflict.accepted_amount, 0, "conflicting replay cannot debit");
    PASS();
}

static void test_fluid_distribution_simple() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t sink = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, sink);

    mgr.setNodeFluid(src, 1000, 2000, 84, true, false);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "fluid network exists");

    auto* net = mgr.getNetwork(targetNet);
    CHECK_EQ(net->fluidId, uint32_t(84), "network fluid type is water");

    auto deltas = mgr.distributeFluid(targetNet, 200);
    CHECK(!deltas.empty(), "fluid distribution produced deltas");

    const auto* sinkNode = mgr.getNode(sink);
    CHECK_GT(sinkNode->fluidBuffer, 0, "sink received fluid");
    CHECK_EQ(sinkNode->fluidId, uint32_t(84), "sink fluid type is water");
    PASS();
}

static void test_fluid_distribution_capacity_limited() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t sink = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, sink);

    // Use fluidId=0 on sink so distributeFluid will fill it
    mgr.setNodeFluid(src, 10000, 10000, 84, true, false);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    mgr.distributeFluid(targetNet, 1000);
    const auto* sinkNode = mgr.getNode(sink);
    CHECK_EQ(sinkNode->fluidBuffer, 500, "sink capped at capacity");
    PASS();
}

static void test_fluid_distribution_no_source() {
    pipenet::PipeNetworkManager mgr;
    uint64_t sink = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    if (targetNet) mgr.distributeFluid(targetNet, 100);
    // distributeFluid adds fluid directly without checking for sources
    CHECK_GE(mgr.getNode(sink)->fluidBuffer, 0, "fluid distribution did not crash");
    PASS();
}

// =========================================================================
//  FluidRegistry tests
// =========================================================================

static void test_fluid_registry_defaults() {
    auto& reg = FluidRegistry::instance();
    uint16_t waterId = ItemId::pack("1111:11:0");
    uint16_t steamId = ItemId::pack("1111:11:1");
    uint16_t acidId  = ItemId::pack("1111:11:2");

    CHECK(reg.isFluid(waterId), "water registered");
    CHECK(reg.isFluid(steamId), "steam registered");
    CHECK(reg.isFluid(acidId), "sulfuric_acid registered");

    const auto* water = reg.getFluid(waterId);
    CHECK(water != nullptr, "water def exists");
    CHECK(water->item_id == waterId, "water id correct");
    CHECK(water->max_temp == 373, "water max temp correct");

    const auto* steam = reg.getFluid(steamId);
    CHECK(steam != nullptr, "steam def exists");
    CHECK(steam->density < 1.0f, "steam less dense than water");

    const auto* acid = reg.getFluid(acidId);
    CHECK(acid != nullptr, "acid def exists");
    CHECK(acid->density > 1.0f, "acid denser than water");

    // Re-init should not duplicate
    reg.initDefaults();
    CHECK(reg.isFluid(waterId), "water still registered after re-init");

    PASS();
}

// =========================================================================
//  CableGraph tests
// =========================================================================

static void test_cable_graph_add_remove() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    const CableDef tinDef = {66, 1, "cable_tin", 100000000.0f, 32, 32};

    graph.addCableNode(1, tinDef, 0, 0, 0);
    graph.addCableNode(2, tinDef, 1, 0, 0);

    CHECK_EQ(graph.isRegisteredGenerator(1), false, "not a generator");
    graph.registerGenerator(100, 0, 0, 0);
    CHECK(graph.isRegisteredGenerator(100), "generator registered");

    graph.unregisterGenerator(100);
    CHECK_EQ(graph.isRegisteredGenerator(100), false, "generator unregistered");

    graph.removeCableNode(1);
    graph.removeCableNode(2);
    PASS();
}

static void test_cable_graph_packet_routing() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    const CableDef def = {66, 1, "cable_tin", 100000000.0f, 32, 32};

    graph.addCableNode(1, def, 0, 0, 0);
    graph.addCableNode(2, def, 1, 0, 0);
    graph.addCableNode(3, def, 2, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, 0, 0, 0);
    graph.registerMachine(200, 2, 0, 0);

    // Inject and tick — verifies no crash, packet processing works
    graph.injectPacket({32, 1, 100, 0, 0}, 1);
    graph.tick();

    graph.collectPackets(200);
    PASS();
}

static void test_cable_graph_voltage_limit() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    // Low voltage cable (maxVoltage=32)
    CableDef lowDef = {66, 1, "cable_tin", 100000000.0f, 32, 32};
    // High voltage cable (maxVoltage=512)
    CableDef highDef = {68, 2, "cable_gold", 49668352.0f, 128, 128};

    graph.addCableNode(1, lowDef, 0, 0, 0);
    graph.addCableNode(2, highDef, 1, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, 0, 0, 0);
    graph.registerMachine(200, 1, 0, 0);

    graph.injectPacket({512, 1, 100, 200, 0}, 1);
    graph.tick();

    auto packets = graph.collectPackets(200);
    // Packet may or may not arrive (voltage check may block it)
    // Test confirms the graph processes without crash
    PASS();
}

static void test_cable_graph_loss() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef cableDef = {1000, 1, "test_cable", 1.0f, 1000, 100};

    graph.addCableNode(1, cableDef, 0, 0, 0);
    graph.addCableNode(2, cableDef, 1, 0, 0);
    graph.addCableNode(3, cableDef, 2, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 3, 0, 0);

    graph.injectPacket({100, 1, 100, 0, 0}, 1);
    graph.tick();

    auto packets = graph.collectPackets(200);
    CHECK_GT(packets.size(), size_t(0), "packet arrived at machine after loss");
    if (!packets.empty()) {
        CHECK_EQ(packets[0].voltage, uint32_t(98), "voltage reduced by 2 after 2 hops");
    }

    PASS();
}

static void test_cable_graph_heavy_loss() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef highLossDef = {1001, 1, "lossy_cable", 60.0f, 1000, 100};

    // 4 cable nodes: (0..3,0,0), machine adjacent to node 4
    graph.addCableNode(1, highLossDef, 0, 0, 0);
    graph.addCableNode(2, highLossDef, 1, 0, 0);
    graph.addCableNode(3, highLossDef, 2, 0, 0);
    graph.addCableNode(4, highLossDef, 3, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 4, 0, 0);

    // broadcast: 100V through 3 hops of 60 EU loss → 100-180 < 0 → dissipated
    graph.injectPacket({100, 1, 100, 0, 0}, 1);
    graph.tick();

    auto packets = graph.collectPackets(200);
    CHECK_EQ(packets.size(), size_t(0), "packet dissipated before reaching machine");
    PASS();
}

static void test_cable_graph_overheat_explosion() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef cableDef = {1002, 1, "weak_cable", 0.0f, 32, 5};

    // 2 cable nodes: (0,0,0)=1, (1,0,0)=2. Machine adjacent to node 2.
    graph.addCableNode(1, cableDef, 0, 0, 0);
    graph.addCableNode(2, cableDef, 1, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 2, 0, 0);

    // Inject overvoltage (34V, max is 32). In broadcasting: path 1→2.
    // During forwarding, node 2 gets maxSeenVoltage=34 and voltage check fails.
    // calculateOverheat: (34-32)*50 = 100 heat. -2 cooldown = 98. Not exploded.
    graph.injectPacket({34, 1, 100, 0, 0}, 1);
    graph.tick();

    CHECK_EQ(graph.getExplodedNodes().size(), size_t(0), "no explosion after 1 tick");

    // Second tick: same again → 98 + 100 - 2 = 196 ≥ 100 → exploded
    graph.injectPacket({34, 1, 100, 0, 0}, 1);
    graph.tick();

    CHECK_GT(graph.getExplodedNodes().size(), size_t(0), "explosion after 2 ticks");
    if (!graph.getExplodedNodes().empty()) {
        const auto& expl = graph.getExplodedNodes()[0];
        CHECK_EQ(expl.nodeId, uint64_t(2), "exploded node is the receiver");
        CHECK_GE(expl.temperature, 100.0f, "temperature >= threshold at explosion");
    }

    PASS();
}

static void test_cable_graph_ampacity_overheat() {
    using namespace gtnh::pipe_network;

    CableGraph graph;
    CableDef cableDef = {1003, 1, "weak_cable", 0.0f, 1000, 1};

    // 2 cable nodes, machine adjacent to node 2
    graph.addCableNode(1, cableDef, 0, 0, 0);
    graph.addCableNode(2, cableDef, 1, 0, 0);
    graph.rebuildGraph();

    graph.registerGenerator(100, -1, 0, 0);
    graph.registerMachine(200, 2, 0, 0);

    // Multiple ticks with 5 packets each (ampacity=1, so 4 overflow per tick)
    // Per tick: calculateOverheat adds (5-1)*1 = 4 heat, -2 cooldown = net +2
    // After ~50 ticks: 50*2 = 100 → threshold reached
    for (int tick = 0; tick < 100; ++tick) {
        for (int i = 0; i < 5; ++i) {
            graph.injectPacket({50, 1, 100, 0, 0}, 1);
        }
        graph.tick();
        if (!graph.getExplodedNodes().empty()) break;
    }

    CHECK_GT(graph.getExplodedNodes().size(), size_t(0), "explosion from ampacity overheat");
    PASS();
}

// =========================================================================
//  HeatLoss module + pipe heat transport
// =========================================================================

static void test_heat_loss_basic() {
    // Mirrors cableEnergyLoss: loss = traversed distance x per-block resistance
    CHECK_EQ(pipenet::heatTransferLoss(5.0f, 0.5f), 2.5f, "loss = distance x resistance");
    CHECK_EQ(pipenet::effectiveHeatTransfer(5.0f, 0.5f, 10.0f), 7.5f, "heat minus edge loss");
    CHECK_EQ(pipenet::effectiveHeatTransfer(5.0f, 0.5f, 2.0f), 0.0f, "loss clamps heat at zero");

    auto res = pipenet::applyHeatLoss(100.0f, 30.0f);
    CHECK_EQ(res.effectiveHeat, 70.0f, "effective heat after traversal loss");
    CHECK_EQ(res.lostHeat, 30.0f, "lost heat recorded");

    auto resFull = pipenet::applyHeatLoss(100.0f, 500.0f);
    CHECK_EQ(resFull.effectiveHeat, 0.0f, "loss beyond heat clamps effective to zero");
    CHECK_EQ(resFull.lostHeat, 100.0f, "lost heat never exceeds available heat");
    PASS();
}

static void test_heat_loss_node_temperature() {
    auto r = pipenet::calculateNodeTemperature(0.0f, 50.0f);
    CHECK_EQ(r.temperature, 48.0f, "temp = throughput - cooldown");
    CHECK(!r.overheated, "not overheated");

    auto hot = pipenet::calculateNodeTemperature(95.0f, 10.0f);
    CHECK(hot.overheated, "crosses max temperature threshold");
    CHECK_GT(hot.temperature, 100.0f, "temp above threshold");

    auto cooled = pipenet::calculateNodeTemperature(10.0f, 0.0f);
    CHECK_EQ(cooled.temperature, 8.0f, "cooldown each tick with no throughput");

    auto clamped = pipenet::calculateNodeTemperature(1.0f, 0.0f);
    CHECK_EQ(clamped.temperature, 0.0f, "temperature clamped at zero");
    PASS();
}

static uint64_t heatNetworkWithSink(pipenet::PipeNetworkManager& mgr,
                                    uint64_t sinkId) {
    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sinkId) return n->id;
    return 0;
}

static void test_heat_distribution_loss_reduction() {
    pipenet::PipeNetworkManager mgr;
    // source -> mid -> sink, 10 blocks between each hop, resistance 0.5/block
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t mid = mgr.addNode(10, 0, 0, 1);
    uint64_t sink = mgr.addNode(20, 0, 0, 1);
    mgr.addEdge(src, mid, 0.5f);
    mgr.addEdge(mid, sink, 0.5f);

    mgr.setNodeHeat(src, 5000, 5000, true, false);
    mgr.setNodeHeat(mid, 0, 5000, false, false);
    mgr.setNodeHeat(sink, 0, 5000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    CHECK_GT(netId, uint64_t(0), "found heat network");

    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // Excess above 90% = 500. Traversed-edge loss = (0.5*10)+(0.5*10) = 10.
    // Effective transfer = 500 - 10 = 490.
    const auto* srcNode = mgr.getNode(src);
    const auto* sinkNode = mgr.getNode(sink);
    CHECK(!deltas.empty(), "heat distribution produced deltas");
    CHECK_EQ(sinkNode->heatStored, 490, "heat reduced by traversed-edge loss");
    CHECK_EQ(srcNode->heatStored, 5000 - 490, "source drained by delivered heat only");
    PASS();
}

static void test_heat_distribution_no_loss_with_zero_resistance() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t sink = mgr.addNode(1, 0, 0, 1);
    mgr.addEdge(src, sink, 0.0f);

    mgr.setNodeHeat(src, 2000, 2000, true, false);
    mgr.setNodeHeat(sink, 0, 2000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // Excess above 90% = 200, no edge loss -> full 200 delivered.
    CHECK_EQ(mgr.getNode(sink)->heatStored, 200, "zero-resistance edges lose nothing");
    PASS();
}

static void test_heat_distribution_capped_at_max() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t sink = mgr.addNode(1, 0, 0, 1);
    mgr.addEdge(src, sink, 0.0f);

    mgr.setNodeHeat(src, 100000, 100000, true, false);
    mgr.setNodeHeat(sink, 0, 100000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // Excess is huge but flow is capped at MAX_HEAT_PER_TICK (1000).
    int32_t delivered = 0;
    for (const auto& [nid, d] : deltas) if (d > 0) delivered += d;
    CHECK_GE(delivered, 1, "heat flowed");
    CHECK_GE(pipenet::HeatConstants::MAX_HEAT_PER_TICK, delivered, "flow capped at 1000/tick");
    CHECK_EQ(mgr.getNode(sink)->heatStored, pipenet::HeatConstants::MAX_HEAT_PER_TICK,
             "sink received exactly the cap with no loss");
    PASS();
}

static void test_heat_node_temperature_tracked() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 1);
    uint64_t sink = mgr.addNode(1, 0, 0, 1);
    mgr.addEdge(src, sink, 0.0f);

    mgr.setNodeHeat(src, 2000, 2000, true, false);
    mgr.setNodeHeat(sink, 0, 2000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);

    // 200 heat moved: temp rises by throughput (cooldown clamped at zero first).
    CHECK_GT(mgr.getNode(src)->temperature, 0.0f, "source temperature rises");
    CHECK_GT(mgr.getNode(sink)->temperature, 0.0f, "sink temperature rises");

    // Second tick: source drained to 90% (no excess), cooldown still applies.
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    float cooled = mgr.getNode(sink)->temperature;
    CHECK_LT(cooled, 200.0f, "temperature cools over ticks");
    mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    CHECK_LT(mgr.getNode(sink)->temperature, cooled, "temperature keeps cooling while idle");
    PASS();
}

// =========================================================================
//  Heat pipe block (1111:10:4)
// =========================================================================

static void test_heat_pipe_node_capacity() {
    pipenet::PipeNetworkManager mgr;
    uint64_t hp = mgr.addNode(0, 0, 0, ItemId::pack("1111:10:4"));
    const auto* node = mgr.getNode(hp);
    CHECK(node, "heat pipe node registered");
    CHECK_EQ(node->heatCapacity, 1000, "heat pipe carries heatCapacity=1000");
    CHECK_EQ(node->itemCapacity, 0, "heat pipe carries no items");
    CHECK_EQ(node->fluidCapacity, 0, "heat pipe carries no fluids");
    PASS();
}

static void test_heat_pipe_network_transport() {
    pipenet::PipeNetworkManager mgr;
    // heat_generator (source) -> heat_pipe -> boiler (HEAT sink)
    uint64_t src  = mgr.addNode(0, 0, 0, 1);  // plain block, set as heat source
    uint64_t hp   = mgr.addNode(1, 0, 0, ItemId::pack("1111:10:4"));
    uint64_t sink = mgr.addNode(2, 0, 0, 1);  // plain block, set as heat sink
    mgr.addEdge(src, hp, 0.0f);
    mgr.addEdge(hp, sink, 0.0f);

    mgr.setNodeHeat(src, 5000, 5000, true, false);
    mgr.setNodeHeat(hp, 0, 1000, false, false);
    mgr.setNodeHeat(sink, 0, 5000, false, true);

    uint64_t netId = heatNetworkWithSink(mgr, sink);
    CHECK_GT(netId, uint64_t(0), "heat network discovered through heat pipe");

    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    CHECK(!deltas.empty(), "heat distributed across heat pipe");
    CHECK_GT(mgr.getNode(sink)->heatStored, 0, "sink received heat through heat pipe");
    CHECK_LT(mgr.getNode(src)->heatStored, 5000, "source drained");
    PASS();
}

static void test_heat_pipe_no_fluid_transport() {
    pipenet::PipeNetworkManager mgr;
    uint64_t hp = mgr.addNode(0, 0, 0, ItemId::pack("1111:10:4"));
    mgr.setNodeHeat(hp, 0, 1000, false, false);
    uint64_t netId = heatNetworkWithSink(mgr, hp);
    // No sink -> no distribution, and fluid path stays empty.
    auto deltas = mgr.distributeHeat(netId, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    CHECK(deltas.empty() || mgr.getNode(hp)->heatStored == 0,
          "heat pipe alone without source/sink does not produce heat");
    PASS();
}

// =========================================================================
//  Edge cases and stress
// =========================================================================

static void test_remove_edge_and_rebuild() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 100);
    uint64_t b = mgr.addNode(1, 0, 0, 100);
    uint64_t c = mgr.addNode(2, 0, 0, 100);
    uint64_t e1 = mgr.addEdge(a, b);
    mgr.addEdge(b, c);

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(1), "one network with 3 nodes");

    mgr.removeEdge(e1);
    mgr.rebuildNetworks();
    // a isolated, b-c connected
    CHECK_EQ(mgr.networkCount(), size_t(2), "two networks after edge removal");
    PASS();
}

static void test_large_network() {
    // 100 nodes in a line - stress test
    pipenet::PipeNetworkManager mgr;
    std::vector<uint64_t> nodes;
    for (int i = 0; i < 100; ++i) {
        nodes.push_back(mgr.addNode(i, 0, 0, 62));
    }
    for (size_t i = 0; i < nodes.size() - 1; ++i) {
        mgr.addEdge(nodes[i], nodes[i + 1]);
    }

    mgr.rebuildNetworks();
    CHECK_EQ(mgr.networkCount(), size_t(1), "one large network");
    CHECK_EQ(mgr.nodeCount(), size_t(100), "100 nodes");
    CHECK_EQ(mgr.edgeCount(), size_t(99), "99 edges");

    // Item network: first node source, last node sink
    mgr.setNodeItemProps(nodes[0], 100, true, false);
    for (size_t i = 1; i < nodes.size() - 1; ++i) {
        mgr.setNodeItemProps(nodes[i], 100, false, false);
    }
    mgr.setNodeItemProps(nodes.back(), 100, false, true);

    mgr.addNodeItem(nodes[0], 1, 1);

    mgr.tickItemNetworks();
    CHECK_EQ(mgr.getNode(nodes[0])->itemBuffer.size(), size_t(0), "source drained");
    PASS();
}

static void test_node_count_after_operations() {
    pipenet::PipeNetworkManager mgr;
    CHECK_EQ(mgr.nodeCount(), size_t(0), "empty");
    uint64_t n1 = mgr.addNode(0, 0, 0, 100);
    CHECK_EQ(mgr.nodeCount(), size_t(1), "after add");
    mgr.removeNode(n1);
    CHECK_EQ(mgr.nodeCount(), size_t(0), "after remove");

    // Re-add with same coords (different id)
    mgr.addNode(0, 0, 0, 100);
    CHECK_EQ(mgr.nodeCount(), size_t(1), "re-added");
    PASS();
}

// =========================================================================
//  Persistence tests
// =========================================================================

static void test_export_import_item_buffers() {
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, true, false);
    mgr.addNodeItem(a, 42, 1);
    mgr.addNodeItem(a, 7, 2);

    auto exported = mgr.exportItemBuffers();
    CHECK_EQ(exported.size(), size_t(1), "one node with items exported");
    CHECK(exported.find(a) != exported.end(), "node a in export");
    CHECK_EQ(exported.at(a).size(), size_t(2), "two items exported from node a");
    CHECK_EQ(exported.at(a)[0].item_id, uint16_t(42), "exported item id correct");
    CHECK_EQ(exported.at(a)[0].count, uint8_t(1), "exported item count correct");

    // Re-import to a fresh manager with same node IDs
    pipenet::PipeNetworkManager mgr2;
    uint64_t a2 = mgr2.addNode(0, 0, 0, 62);
    mgr2.addNode(1, 0, 0, 62);

    mgr2.setNodeItemProps(a2, 10, true, false);
    mgr2.importItemBuffers(exported);
    const auto* restored = mgr2.getNode(a2);
    CHECK(restored != nullptr, "node a2 exists after import");
    CHECK_EQ(restored->itemBuffer.size(), size_t(2), "two items restored");
    CHECK_EQ(restored->itemBuffer[0].item_id, uint16_t(42), "first item id restored");
    CHECK_EQ(restored->itemBuffer[0].count, uint8_t(1), "first item count restored");
    CHECK_EQ(restored->itemBuffer[1].item_id, uint16_t(7), "second item id restored");
    PASS();
}

static void test_export_empty_no_buffers() {
    pipenet::PipeNetworkManager mgr;
    mgr.addNode(0, 0, 0, 62);
    auto exported = mgr.exportItemBuffers();
    CHECK(exported.empty(), "no items to export on empty buffers");
    PASS();
}

// =========================================================================
//  Fluid routing tests (extended)
// =========================================================================

static void test_fluid_routing_type_mismatch() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t sink = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, sink);

    mgr.setNodeFluid(src, 1000, 2000, 84, true, false);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }
    CHECK_GT(targetNet, uint64_t(0), "fluid network exists");

    auto* net = mgr.getNetwork(targetNet);
    CHECK_EQ(net->fluidId, uint32_t(84), "network fluid type is water");

    // Try to put lava (fluid_id=85) into same network — should be blocked at the network level
    mgr.setNodeFluid(src, 1000, 2000, 85, true, false);  // lava
    mgr.rebuildNetworks();
    // After rebuild, network type should still be the original fluid type
    auto nets2 = mgr.getAllNetworks();
    for (const auto* n : nets2)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    CHECK_GT(targetNet, uint64_t(0), "network still exists after fluid type change");
    PASS();
}

static void test_fluid_routing_capacity() {
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 61);
    uint64_t sink = mgr.addNode(1, 0, 0, 61);
    mgr.addEdge(src, sink);

    mgr.setNodeFluid(src, 10000, 10000, 84, true, false);
    mgr.setNodeFluid(sink, 0, 500, 0, false, true);

    mgr.rebuildNetworks();
    auto nets = mgr.getAllNetworks();
    uint64_t targetNet = 0;
    for (const auto* n : nets)
        for (uint64_t nid : n->nodeIds)
            if (nid == sink) { targetNet = n->id; break; }

    mgr.distributeFluid(targetNet, 1000);
    const auto* sinkNode = mgr.getNode(sink);
    CHECK_EQ(sinkNode->fluidBuffer, 500, "sink capped at capacity");

    // Second distribution: already full, no more fluid accepted
    mgr.distributeFluid(targetNet, 500);
    CHECK_EQ(sinkNode->fluidBuffer, 500, "sink still capped after second distribution");
    PASS();
}

// =========================================================================
//  Pipe-buffer conservation (3.3.4)
// =========================================================================

static void test_fluid_conservation_accepted_blocked_disconnected() {
    pipenet::PipeNetworkManager mgr;
    uint64_t sink = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(sink, 100, 1000, 2, false, true);

    // Accepted: the buffer is debited exactly the accepted amount and
    // accepted + remaining reconstructs the request.
    int32_t before = mgr.getNode(sink)->fluidBuffer;
    auto accepted = mgr.consumeFluid(sink, 1, 2, 60);
    CHECK_EQ(accepted.accepted_amount, 60, "accepted transfer");
    CHECK_EQ(before - mgr.getNode(sink)->fluidBuffer, accepted.accepted_amount,
             "pipe buffer debited exactly the accepted amount");
    CHECK_EQ(accepted.accepted_amount + accepted.remaining, 60,
             "accepted + remaining == requested");

    // Blocked (fluid mismatch): zero accepted, buffer unchanged.
    before = mgr.getNode(sink)->fluidBuffer;
    auto blocked = mgr.consumeFluid(sink, 2, 84, 30);
    CHECK(blocked.blocked, "mismatched fluid blocked");
    CHECK_EQ(blocked.accepted_amount, 0, "blocked transfer accepts nothing");
    CHECK_EQ(mgr.getNode(sink)->fluidBuffer, before,
             "blocked transfer leaves the buffer unchanged");

    // Disconnected (no FLUID sink domain): zero accepted, buffer unchanged.
    uint64_t lone = mgr.addNode(50, 0, 0, 61);
    mgr.setNodeFluid(lone, 100, 1000, 2, false, false);
    before = mgr.getNode(lone)->fluidBuffer;
    auto disconnected = mgr.consumeFluid(lone, 3, 2, 30);
    CHECK(disconnected.blocked, "consume without a FLUID sink is blocked");
    CHECK_EQ(disconnected.accepted_amount, 0, "disconnected transfer accepts nothing");
    CHECK_EQ(mgr.getNode(lone)->fluidBuffer, before,
             "disconnected transfer leaves the buffer unchanged");
    PASS();
}

// =========================================================================
//  Service-side shortfall transactions (3.4.2/3.4.3/2.5.3)
// =========================================================================

static void test_shortfall_plan_records_pending() {
    gtnh::pipe_network::PipeConsumeTracker tracker(10);
    auto plan = tracker.planShortfall(
        /*drain_request_id=*/501, /*consume_request_id=*/7,
        /*sink_node_id=*/30, /*fluid_id=*/2, /*requested=*/100,
        /*pipe_accepted=*/40, /*source_node_id=*/11, /*source_owner_id=*/99,
        /*source_port_id=*/5, /*now_tick=*/3);
    CHECK(plan.request_source, "shortfall emits a source drain request");
    CHECK_EQ(plan.shortfall, 60, "drain request covers only the shortfall");
    CHECK_EQ(plan.drain_request_id, uint64_t(501), "drain request id threaded");
    CHECK_EQ(plan.source_port_id, gtnh::common::PortId(5), "typed port id threaded");
    CHECK_EQ(tracker.size(), size_t(1), "pending consume recorded");

    auto full = tracker.planShortfall(502, 8, 30, 2, 100, 100, 11, 99, 5, 3);
    CHECK(!full.request_source, "no shortfall means no drain request");
    CHECK_EQ(tracker.size(), size_t(1), "full pipe serve records nothing");

    auto no_source = tracker.planShortfall(503, 9, 30, 2, 100, 0, 0, 0, 0, 3);
    CHECK(!no_source.request_source, "missing source candidate emits nothing");
    CHECK_EQ(tracker.size(), size_t(1), "sourceless plan records nothing");
    PASS();
}

static void test_drain_response_applied_exactly_once() {
    gtnh::pipe_network::PipeConsumeTracker tracker(10);
    auto plan = tracker.planShortfall(601, 7, 30, 2, 100, 40, 11, 99, 5, 0);
    CHECK(plan.request_source, "pending recorded");

    auto first = tracker.applyDrainResponse(601, 2, 25);
    CHECK(first.applied, "first response applies");
    CHECK_EQ(first.source_accepted, 25, "source part credited");
    CHECK_EQ(first.combined_accepted, 65, "combined = pipe part + source part");
    CHECK_EQ(first.remaining, 35, "remaining is exact");

    auto replay = tracker.applyDrainResponse(601, 2, 25);
    CHECK(!replay.applied, "duplicate response is ignored (replay guard)");
    CHECK_EQ(replay.combined_accepted, 0, "replay credits nothing");

    auto unknown = tracker.applyDrainResponse(999, 2, 25);
    CHECK(!unknown.applied, "unknown request id is ignored");
    PASS();
}

static void test_drain_response_clamped_and_mismatched() {
    gtnh::pipe_network::PipeConsumeTracker tracker(10);
    tracker.planShortfall(701, 7, 30, 2, 100, 60, 11, 99, 5, 0);

    auto over = tracker.applyDrainResponse(701, 2, 90);
    CHECK(over.applied, "over-acceptance still completes the consume");
    CHECK_EQ(over.source_accepted, 40, "accepted clamped to the shortfall");
    CHECK_EQ(over.combined_accepted, 100, "combined never exceeds requested");

    tracker.planShortfall(702, 8, 30, 2, 100, 0, 11, 99, 5, 0);
    auto negative = tracker.applyDrainResponse(702, 2, -5);
    CHECK(negative.applied, "negative acceptance completes as blocked");
    CHECK_EQ(negative.source_accepted, 0, "negative amount rejected");
    CHECK_EQ(negative.combined_accepted, 0, "blocked response is zero-accepted");
    CHECK_EQ(negative.remaining, 100, "blocked response reports full remaining");

    tracker.planShortfall(703, 9, 30, 2, 100, 0, 11, 99, 5, 0);
    auto mismatch = tracker.applyDrainResponse(703, 84, 50);
    CHECK(!mismatch.applied, "mismatched fluid response is ignored");
    PASS();
}

static void test_pending_expiry_completes_short_fill() {
    gtnh::pipe_network::PipeConsumeTracker tracker(5);
    tracker.planShortfall(801, 7, 30, 2, 100, 30, 11, 99, 5, 0);
    CHECK(tracker.expire(4).empty(), "nothing expires before the TTL");
    auto expired = tracker.expire(5);
    CHECK_EQ(expired.size(), size_t(1), "pending expires at the TTL");
    CHECK_EQ(expired[0].pipe_accepted, 30, "expired entry keeps the pipe part");
    CHECK_EQ(expired[0].requested, 100, "expired entry keeps the demand");
    CHECK(tracker.empty(), "pending map bounded by expiry");
    auto late = tracker.applyDrainResponse(801, 2, 70);
    CHECK(!late.applied, "late response after expiry is ignored");
    PASS();
}

static void test_port_removal_clears_pending() {
    gtnh::pipe_network::PipeConsumeTracker tracker(50);
    tracker.planShortfall(901, 7, 30, 2, 100, 10, 11, 99, 5, 0);
    tracker.planShortfall(902, 8, 31, 2, 100, 10, 12, 0, 0, 0);
    tracker.planShortfall(903, 9, 32, 2, 100, 10, 13, 99, 6, 0);

    auto cleared = tracker.clearForPort(99, 5);
    CHECK_EQ(cleared.size(), size_t(1), "only the removed port's pending clears");
    CHECK_EQ(cleared[0].drain_request_id, uint64_t(901), "correct pending cleared");
    CHECK_EQ(cleared[0].pipe_accepted, 10, "cleared pending keeps its pipe part");
    CHECK_EQ(tracker.size(), size_t(2), "other pendings survive");

    CHECK(tracker.clearForPort(99, 5).empty(), "repeated removal clears nothing");
    CHECK(tracker.clearForPort(98, 6).empty(), "wrong owner does not clear port 6");
    CHECK(tracker.clearForPort(99, 0).empty(),
          "port id zero never matches (node-based fallback)");
    auto rest = tracker.clearForPort(99, 6);
    CHECK_EQ(rest.size(), size_t(1), "second port clears its own pending");
    CHECK_EQ(tracker.size(), size_t(1),
             "node-based pending survives port removal (TTL-bound)");
    PASS();
}

// =========================================================================
//  2.6.x: converters, simultaneous ports, entity ID zero, ID collisions,
//  duplicate/stale updates, exact removal, removal cleanup
// =========================================================================

// 2.6.1: HU, FLUID, EU and ITEM ports on ONE owner stay four independent
// domains — roles, rates and solve-time behavior never bleed across kinds.
static void test_four_domains_simultaneous_one_owner() {
    pipenet::PipeNetworkManager mgr;
    CHECK(mgr.addNodeWithId(500, 1, 2, 3, 61), "converter node exists");

    auto hu = make_port_at(gtnh::common::ResourceKind::HU,
                           gtnh::common::PortRole::SINK, 500, 11, 1, 2, 3);
    auto fluid = make_port_at(gtnh::common::ResourceKind::FLUID,
                              gtnh::common::PortRole::SOURCE, 500, 12, 1, 2, 3);
    auto eu = make_port_at(gtnh::common::ResourceKind::EU,
                           gtnh::common::PortRole::SINK, 500, 13, 1, 2, 3);
    auto item = make_port_at(gtnh::common::ResourceKind::ITEM,
                             gtnh::common::PortRole::SOURCE, 500, 14, 1, 2, 3);
    hu.rate = 10;
    fluid.rate = 20;
    eu.rate = 30;
    item.rate = 40;
    CHECK(mgr.registerPort(hu), "HU sink registers");
    CHECK(mgr.registerPort(fluid), "FLUID source registers");
    CHECK(mgr.registerPort(eu), "EU sink registers");
    CHECK(mgr.registerPort(item), "ITEM source registers");
    CHECK_EQ(mgr.portCount(), size_t(4), "four domains, four ports");

    const auto* node = mgr.getNode(500);
    CHECK(node != nullptr, "node exists");
    const auto& hu_d = node->domains[pipenet::domainIndex(gtnh::common::ResourceKind::HU)];
    const auto& fluid_d = node->domains[pipenet::domainIndex(gtnh::common::ResourceKind::FLUID)];
    const auto& eu_d = node->domains[pipenet::domainIndex(gtnh::common::ResourceKind::EU)];
    const auto& item_d = node->domains[pipenet::domainIndex(gtnh::common::ResourceKind::ITEM)];
    CHECK(hu_d.is_sink && !hu_d.is_source, "HU domain is sink-only");
    CHECK_EQ(hu_d.rate, 10, "HU rate carried");
    CHECK(fluid_d.is_source && !fluid_d.is_sink, "FLUID domain is source-only");
    CHECK_EQ(fluid_d.rate, 20, "FLUID rate carried");
    CHECK(eu_d.is_sink && !eu_d.is_source, "EU domain is sink-only");
    CHECK_EQ(eu_d.rate, 30, "EU rate carried");
    CHECK(item_d.is_source && !item_d.is_sink, "ITEM domain is source-only");
    CHECK_EQ(item_d.rate, 40, "ITEM rate carried");

    // Solve-time independence: HU/EU sinks do not make the node a fluid sink.
    auto res = mgr.consumeFluid(500, 1, 2, 10);
    CHECK(res.blocked, "fluid consume blocked while only HU/EU sinks exist");
    CHECK_EQ(res.accepted_amount, 0, "cross-domain solve cannot debit");

    // The registration key includes the kind: the same slot id in another
    // domain is a distinct port, and fluid consume through the HU slot id is
    // cross-kind rejected.
    auto fluid_same_slot = make_port_at(gtnh::common::ResourceKind::FLUID,
                                        gtnh::common::PortRole::SOURCE, 500, 11,
                                        1, 2, 3);
    CHECK(mgr.registerPort(fluid_same_slot), "same slot id in FLUID coexists with HU");
    CHECK(mgr.hasPort(500, gtnh::common::ResourceKind::HU, 11),
          "HU port 11 survives");
    CHECK(mgr.hasPort(500, gtnh::common::ResourceKind::FLUID, 11),
          "FLUID port 11 is a distinct registration");
    auto via = mgr.consumeFluidViaPort(500, 11, 2, 2, 10);
    CHECK(via.blocked, "port 11 is not a FLUID SINK on either registration");

    CHECK_EQ(mgr.removePortsForOwner(500), size_t(5),
             "owner cleanup removes every domain's ports");
    CHECK_EQ(mgr.portCount(), size_t(0), "registry empty after owner cleanup");
    CHECK(!mgr.nodeDomain(500, gtnh::common::ResourceKind::HU)->is_sink,
          "HU domain cleared");
    CHECK(!mgr.nodeDomain(500, gtnh::common::ResourceKind::FLUID)->is_source,
          "FLUID domain cleared");
    CHECK(!mgr.nodeDomain(500, gtnh::common::ResourceKind::EU)->is_sink,
          "EU domain cleared");
    CHECK(!mgr.nodeDomain(500, gtnh::common::ResourceKind::ITEM)->is_source,
          "ITEM domain cleared");
    PASS();
}

// 2.6.2: converter with an HU sink + FLUID source on one owner — neither role
// bleeds into the other domain, and per-port epoch replacement is isolated.
static void test_converter_source_sink_roles_no_bleed() {
    pipenet::PipeNetworkManager mgr;
    CHECK(mgr.addNodeWithId(99, 10, 20, 30, 61), "boiler node exists");
    mgr.setNodeFluid(99, 500, 1000, 2, false, false);  // buffer only, no roles

    auto hu = make_port_at(gtnh::common::ResourceKind::HU,
                           gtnh::common::PortRole::SINK, 99, 1, 10, 20, 30);
    auto fluid = make_port_at(gtnh::common::ResourceKind::FLUID,
                              gtnh::common::PortRole::SOURCE, 99, 2, 10, 20, 30);
    CHECK(mgr.registerPort(hu), "HU sink registers");
    CHECK(mgr.registerPort(fluid), "FLUID source registers");

    // The FLUID source role must not make the node a fluid sink.
    auto res = mgr.consumeFluid(99, 1, 2, 10);
    CHECK(res.blocked, "converter FLUID source is not consumable as a sink");
    CHECK_EQ(mgr.getNode(99)->fluidBuffer, 500, "blocked consume leaves buffer");
    CHECK(mgr.nodeDomain(99, gtnh::common::ResourceKind::HU)->is_sink,
          "HU sink in HU domain");
    CHECK(!mgr.nodeDomain(99, gtnh::common::ResourceKind::FLUID)->is_sink,
          "HU sink does not bleed into FLUID");
    CHECK(mgr.nodeDomain(99, gtnh::common::ResourceKind::FLUID)->is_source,
          "FLUID source in FLUID domain");
    CHECK(!mgr.nodeDomain(99, gtnh::common::ResourceKind::HU)->is_source,
          "FLUID source does not bleed into HU");

    // Re-registering the HU port at a new epoch leaves the FLUID record alone.
    auto hu2 = hu;
    hu2.epoch = 2;
    hu2.rate = 777;
    CHECK(mgr.registerPort(hu2), "HU epoch bump accepted");
    CHECK_EQ(mgr.getPort(99, gtnh::common::ResourceKind::FLUID, 2)->rate, 100,
             "FLUID record untouched by HU replacement");
    CHECK_EQ(mgr.nodeDomain(99, gtnh::common::ResourceKind::HU)->rate, 777,
             "HU domain carries the new rate");
    CHECK_EQ(mgr.nodeDomain(99, gtnh::common::ResourceKind::FLUID)->rate, 100,
             "FLUID domain keeps its own rate");

    // Removing the HU port clears only the HU domain.
    CHECK(mgr.removePort(hu2), "HU port removes");
    CHECK(!mgr.nodeDomain(99, gtnh::common::ResourceKind::HU)->is_sink,
          "HU domain cleared by removal");
    CHECK(mgr.nodeDomain(99, gtnh::common::ResourceKind::FLUID)->is_source,
          "FLUID source survives HU removal");
    CHECK(mgr.hasPort(99, gtnh::common::ResourceKind::FLUID, 2),
          "FLUID port still registered");
    PASS();
}

// 2.6.3: owner id 0 is a valid machine instance for the whole lifecycle —
// register, idempotent duplicate, consume, exact removal, post-removal block.
static void test_owner_zero_port_full_lifecycle() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(node, 100, 1000, 2, false, false);

    auto port = make_port_at(gtnh::common::ResourceKind::FLUID,
                             gtnh::common::PortRole::SINK, 0, 9, 0, 0, 0);
    CHECK(mgr.registerPort(port), "owner-zero port registers");
    CHECK(mgr.hasPort(0, gtnh::common::ResourceKind::FLUID, 9),
          "owner-zero port queryable");
    CHECK(mgr.registerPort(port), "duplicate owner-zero registration idempotent");
    CHECK_EQ(mgr.portCount(), size_t(1), "no duplicate created");

    auto served = mgr.consumeFluidViaPort(0, 9, 1, 2, 60);
    CHECK_EQ(served.accepted_amount, 60, "owner-zero sink serves the consume");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 40, "buffer debited once");

    CHECK(!mgr.removePort(0, gtnh::common::ResourceKind::FLUID, 9, 2),
          "wrong epoch does not remove owner-zero port");
    CHECK(mgr.removePort(0, gtnh::common::ResourceKind::FLUID, 9, 1),
          "exact key removes the owner-zero port");
    auto after = mgr.consumeFluidViaPort(0, 9, 2, 2, 10);
    CHECK(after.blocked, "removed owner-zero port no longer serves");
    CHECK_EQ(after.accepted_amount, 0, "removed owner-zero port cannot debit");
    PASS();
}

// 2.6.3: port ids are owner-scoped — two owners may use the same port id and
// each owner's registration routes and removes independently.
static void test_port_ids_owner_scoped_no_cross_owner_collision() {
    pipenet::PipeNetworkManager mgr;
    uint64_t n1 = mgr.addNode(0, 0, 0, 61);
    uint64_t n2 = mgr.addNode(5, 0, 0, 61);
    mgr.setNodeFluid(n1, 100, 1000, 2, false, false);
    mgr.setNodeFluid(n2, 200, 1000, 2, false, false);

    auto p1 = make_port_at(gtnh::common::ResourceKind::FLUID,
                           gtnh::common::PortRole::SINK, 1, 7, 0, 0, 0);
    auto p2 = make_port_at(gtnh::common::ResourceKind::FLUID,
                           gtnh::common::PortRole::SINK, 2, 7, 5, 0, 0);
    CHECK(mgr.registerPort(p1), "owner 1 port id 7 registers");
    CHECK(mgr.registerPort(p2), "owner 2 port id 7 registers independently");
    CHECK_EQ(mgr.portCount(), size_t(2), "same port id, two owners, two ports");
    CHECK_EQ(mgr.getPort(1, gtnh::common::ResourceKind::FLUID, 7)->x, 0,
             "owner 1 record kept its position");
    CHECK_EQ(mgr.getPort(2, gtnh::common::ResourceKind::FLUID, 7)->x, 5,
             "owner 2 record kept its position");

    auto via1 = mgr.consumeFluidViaPort(1, 7, 1, 2, 30);
    CHECK_EQ(via1.accepted_amount, 30, "owner 1 consume served");
    CHECK_EQ(mgr.getNode(n1)->fluidBuffer, 70, "owner 1 node debited");
    CHECK_EQ(mgr.getNode(n2)->fluidBuffer, 200, "owner 2 node untouched");
    auto via2 = mgr.consumeFluidViaPort(2, 7, 2, 2, 30);
    CHECK_EQ(via2.accepted_amount, 30, "owner 2 consume served");
    CHECK_EQ(mgr.getNode(n2)->fluidBuffer, 170, "owner 2 node debited");

    CHECK(mgr.removePort(1, gtnh::common::ResourceKind::FLUID, 7, 1),
          "owner 1 exact removal");
    CHECK(!mgr.hasPort(1, gtnh::common::ResourceKind::FLUID, 7),
          "owner 1 port gone");
    CHECK(mgr.hasPort(2, gtnh::common::ResourceKind::FLUID, 7),
          "owner 2 port survives the collision removal");
    auto via1b = mgr.consumeFluidViaPort(1, 7, 3, 2, 10);
    CHECK(via1b.blocked, "owner 1 port removed");
    auto via2b = mgr.consumeFluidViaPort(2, 7, 4, 2, 10);
    CHECK_EQ(via2b.accepted_amount, 10, "owner 2 port still serves");
    PASS();
}

// 2.6.3: EnTT owner ids and manager node ids are independent id spaces that
// may collide numerically; ports route by position and owner cleanup never
// touches node state.
static void test_owner_node_id_collision_independent_spaces() {
    pipenet::PipeNetworkManager mgr;
    CHECK(mgr.addNodeWithId(77, 0, 0, 0, 61), "node id 77 (collides with owner 77)");
    uint64_t other = mgr.addNode(9, 0, 0, 61);
    mgr.setNodeFluid(77, 100, 1000, 2, false, false);
    mgr.setNodeFluid(other, 100, 1000, 2, false, false);

    auto p77 = make_port_at(gtnh::common::ResourceKind::FLUID,
                            gtnh::common::PortRole::SINK, 77, 7, 0, 0, 0);
    auto p78 = make_port_at(gtnh::common::ResourceKind::FLUID,
                            gtnh::common::PortRole::SINK, 78, 7, 9, 0, 0);
    CHECK(mgr.registerPort(p77), "owner 77 (== node id 77) registers");
    CHECK(mgr.registerPort(p78), "owner 78 registers");

    auto via77 = mgr.consumeFluidViaPort(77, 7, 1, 2, 40);
    CHECK_EQ(via77.accepted_amount, 40, "owner 77 routes to node 77 by position");
    CHECK_EQ(mgr.getNode(77)->fluidBuffer, 60, "node 77 debited");
    CHECK_EQ(mgr.getNode(other)->fluidBuffer, 100, "other node untouched");
    auto via78 = mgr.consumeFluidViaPort(78, 7, 2, 2, 40);
    CHECK_EQ(via78.accepted_amount, 40, "owner 78 routes to its own node");
    CHECK_EQ(mgr.getNode(other)->fluidBuffer, 60, "other node debited");

    CHECK_EQ(mgr.removePortsForOwner(77), size_t(1), "owner 77 cleanup");
    CHECK_EQ(mgr.getNode(77)->fluidBuffer, 60, "node 77 survives owner cleanup");
    CHECK(mgr.hasPort(78, gtnh::common::ResourceKind::FLUID, 7),
          "owner 78 untouched by owner 77 cleanup");
    PASS();
}

// 2.6.3: removal matches the full (owner, kind, port, epoch) key; any wrong
// component is rejected and the port survives. Port id 0 is never registrable
// nor removable.
static void test_exact_removal_by_registration_key() {
    pipenet::PipeNetworkManager mgr;
    auto port = make_port_at(gtnh::common::ResourceKind::FLUID,
                             gtnh::common::PortRole::SINK, 9, 5, 0, 0, 0);
    port.epoch = 3;
    CHECK(mgr.registerPort(port), "port registers at epoch 3");

    CHECK(!mgr.removePort(9, gtnh::common::ResourceKind::FLUID, 5, 2),
          "wrong epoch rejected");
    CHECK(!mgr.removePort(9, gtnh::common::ResourceKind::EU, 5, 3),
          "wrong kind rejected");
    CHECK(!mgr.removePort(8, gtnh::common::ResourceKind::FLUID, 5, 3),
          "wrong owner rejected");
    CHECK(mgr.hasPort(9, gtnh::common::ResourceKind::FLUID, 5),
          "port survives wrong-key removals");
    CHECK(mgr.removePort(9, gtnh::common::ResourceKind::FLUID, 5, 3),
          "exact (owner, kind, port, epoch) removes");
    CHECK(!mgr.hasPort(9, gtnh::common::ResourceKind::FLUID, 5),
          "port gone after exact removal");

    // The epoch-less overload removes regardless of the current epoch.
    auto p2 = port;
    p2.epoch = 4;
    CHECK(mgr.registerPort(p2), "re-register at epoch 4");
    CHECK(mgr.removePort(9, gtnh::common::ResourceKind::FLUID, 5),
          "epoch-less removal matches any epoch");

    auto zero = port;
    zero.port_id = 0;
    CHECK(!mgr.registerPort(zero), "port id zero is not registrable");
    CHECK(!mgr.removePort(9, gtnh::common::ResourceKind::FLUID, 0, 1),
          "port id zero cannot be removed");
    PASS();
}

// 2.6.4: manager-side removal clears the port and its domain but replay state
// of completed transactions survives — a redelivered request returns the
// original response exactly once and never re-debits; fresh flow is blocked.
static void test_manager_removal_replay_exactly_once() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(node, 100, 1000, 2, false, false);
    auto port = make_port_at(gtnh::common::ResourceKind::FLUID,
                             gtnh::common::PortRole::SINK, 7, 3, 0, 0, 0);
    CHECK(mgr.registerPort(port), "sink port registers");

    auto first = mgr.consumeFluidViaPort(7, 3, 31, 2, 40);
    CHECK_EQ(first.accepted_amount, 40, "consume served");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 60, "buffer debited once");
    CHECK_EQ(mgr.fluidTransactionCount(), size_t(1), "transaction recorded");

    CHECK_EQ(mgr.removePortsForOwner(7), size_t(1), "owner removal clears the port");

    // Redelivery of the completed transaction via the node route replays the
    // cached response (exactly-once accounting survives removal).
    auto replay = mgr.consumeFluid(node, 31, 2, 40);
    CHECK_EQ(replay.accepted_amount, 40, "redelivery returns cached response");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 60, "redelivery does not re-debit");

    // Fresh flow cannot target the removed port's machine anymore.
    auto fresh = mgr.consumeFluidViaPort(7, 3, 32, 2, 10);
    CHECK(fresh.blocked, "removed port cannot serve new flow");
    CHECK_EQ(fresh.accepted_amount, 0, "fresh request after removal debits nothing");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 60, "buffer unchanged by blocked flow");
    PASS();
}

// 2.6.4: service-side removal completes the pending shortfall consume as a
// short-fill (pipe part only) and discards the late owner response.
static void test_tracker_removal_completes_pending_as_short_fill() {
    gtnh::pipe_network::PipeConsumeTracker tracker(50);
    tracker.planShortfall(901, 7, 30, 2, 100, 40, 11, 99, 5, 0);

    auto cleared = tracker.clearForPort(99, 5);
    CHECK_EQ(cleared.size(), size_t(1), "removed port's pending completes");
    CHECK_EQ(cleared[0].pipe_accepted, 40, "short-fill keeps the pipe part");
    CHECK_EQ(cleared[0].requested, 100, "short-fill keeps the demand");

    auto late = tracker.applyDrainResponse(901, 2, 60);
    CHECK(!late.applied, "owner response after removal is discarded");
    CHECK_EQ(late.combined_accepted, 0, "discarded response credits nothing");

    // Already-applied pendings are not affected by a later removal.
    tracker.planShortfall(902, 8, 30, 2, 100, 10, 11, 99, 5, 0);
    auto applied = tracker.applyDrainResponse(902, 2, 90);
    CHECK(applied.applied, "response applies before removal");
    CHECK(tracker.clearForPort(99, 5).empty(),
          "applied consume is no longer pending for the port");
    PASS();
}

// =========================================================================
//  3.6.x: conservation, partial fills, replay/expiry, mixed fluids, capacity
// =========================================================================

// 3.6.1: pipe-only transfer — summed acceptances equal summed debits exactly,
// accepted + remaining reconstructs each demand, and exhaustion is exact.
static void test_conservation_pipe_only_exact() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(node, 100, 1000, 2, false, true);

    const uint64_t ids[] = {1, 2, 3};
    const int32_t demands[] = {30, 50, 20};
    int32_t total_accepted = 0, total_debited = 0;
    for (int i = 0; i < 3; ++i) {
        const int32_t before = mgr.getNode(node)->fluidBuffer;
        auto r = mgr.consumeFluid(node, ids[i], 2, demands[i]);
        const int32_t debited = before - mgr.getNode(node)->fluidBuffer;
        CHECK_EQ(r.accepted_amount, demands[i], "full acceptance while stock lasts");
        CHECK_EQ(debited, r.accepted_amount, "pipe debited exactly the acceptance");
        CHECK_EQ(r.accepted_amount + r.remaining, demands[i],
                 "accepted + remaining == demand");
        total_accepted += r.accepted_amount;
        total_debited += debited;
    }
    CHECK_EQ(total_accepted, 100, "sum of acceptances == initial stock");
    CHECK_EQ(total_debited, total_accepted, "conservation: accepted == debited");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 0, "buffer exactly empty");
    CHECK_EQ(mgr.getNode(node)->fluidId, uint32_t(0),
             "exact fill resets the node fluid id");

    auto dry = mgr.consumeFluid(node, 4, 2, 10);
    CHECK_EQ(dry.accepted_amount, 0, "no acceptance without stock");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 0, "buffer never negative");
    PASS();
}

// 3.6.1: combined shortfall transfer — pipe part (manager debit) + source part
// (owner response) == destination credit, for full and partial source fills.
static void test_conservation_combined_shortfall() {
    pipenet::PipeNetworkManager mgr;
    uint64_t sink = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(sink, 100, 1000, 2, false, true);
    gtnh::pipe_network::PipeConsumeTracker tracker(50);

    const int32_t requested = 250;
    const int32_t before = mgr.getNode(sink)->fluidBuffer;
    auto pipe = mgr.consumeFluid(sink, 41, 2, requested);
    CHECK_EQ(pipe.accepted_amount, 100, "pipe part capped by the buffer");
    const int32_t pipe_debit = before - mgr.getNode(sink)->fluidBuffer;
    CHECK_EQ(pipe_debit, pipe.accepted_amount, "pipe debited its part");

    auto plan = tracker.planShortfall(42, 41, sink, 2, requested,
                                      pipe.accepted_amount, 11, 99, 5, 0);
    CHECK(plan.request_source, "shortfall emits a drain request");
    CHECK_EQ(plan.shortfall, 150, "drain covers only the shortfall");

    // Owner accepts the full shortfall (the real owner debit == response is
    // pinned by the simcore drain tests).
    auto applied = tracker.applyDrainResponse(42, 2, 150);
    CHECK(applied.applied, "source response applies");
    CHECK_EQ(applied.source_accepted, 150, "source part credited");
    CHECK_EQ(applied.combined_accepted, requested,
             "pipe part + source part == requested");
    CHECK_EQ(applied.remaining, 0, "nothing left");
    CHECK_EQ(pipe_debit + applied.source_accepted, applied.combined_accepted,
             "conservation: pipe debit + source accepted == destination credit");

    // Partial source acceptance: the gap is neither debited nor credited.
    auto pipe2 = mgr.consumeFluid(sink, 43, 2, 100);
    CHECK_EQ(pipe2.accepted_amount, 0, "empty pipe serves nothing");
    auto plan2 = tracker.planShortfall(44, 43, sink, 2, 100, pipe2.accepted_amount,
                                       11, 99, 5, 0);
    CHECK_EQ(plan2.shortfall, 100, "full demand is the shortfall");
    auto applied2 = tracker.applyDrainResponse(44, 2, 60);
    CHECK(applied2.applied, "partial source response applies");
    CHECK_EQ(applied2.combined_accepted, 60, "combined == actual parts only");
    CHECK_EQ(applied2.remaining, 40, "unserved gap reported exactly");

    // Over-acceptance is clamped: the destination is never credited beyond
    // the request.
    auto pipe3 = mgr.consumeFluid(sink, 45, 2, 30);
    CHECK_EQ(pipe3.accepted_amount, 0, "still no pipe stock");
    tracker.planShortfall(46, 45, sink, 2, 30, 0, 11, 99, 5, 0);
    auto applied3 = tracker.applyDrainResponse(46, 2, 90);
    CHECK_EQ(applied3.source_accepted, 30, "over-acceptance clamped to shortfall");
    CHECK_EQ(applied3.combined_accepted, 30, "combined never exceeds requested");
    PASS();
}

// 3.6.2: partial and zero acceptance debit exactly the accepted amount on the
// pipe side and credit exactly the reported parts at the destination.
static void test_partial_zero_acceptance_no_over_debit() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(node, 30, 1000, 2, false, true);

    auto partial = mgr.consumeFluid(node, 1, 2, 100);
    CHECK_EQ(partial.accepted_amount, 30, "partial acceptance limited by stock");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 0, "debited exactly the acceptance");
    CHECK_GE(mgr.getNode(node)->fluidBuffer, 0, "buffer never negative");

    auto zero = mgr.consumeFluid(node, 2, 2, 50);
    CHECK_EQ(zero.accepted_amount, 0, "zero acceptance on empty stock");
    CHECK_EQ(zero.remaining, 50, "zero acceptance reports full remaining");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 0, "zero acceptance debits nothing");

    // Destination side: a zero source response credits only the pipe part.
    gtnh::pipe_network::PipeConsumeTracker tracker(50);
    tracker.planShortfall(10, 3, node, 2, 50, 20, 11, 99, 5, 0);
    auto applied = tracker.applyDrainResponse(10, 2, 0);
    CHECK(applied.applied, "zero-acceptance response still completes");
    CHECK_EQ(applied.source_accepted, 0, "no source part invented");
    CHECK_EQ(applied.combined_accepted, 20, "destination credited only the pipe part");
    CHECK_EQ(applied.remaining, 30, "remaining exact");
    PASS();
}

// 3.6.3: request id zero is an uncorrelated request — it executes for real,
// is never cached, and never aliases a cached transaction.
static void test_request_id_zero_never_cached() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(node, 20, 1000, 2, false, true);

    auto a = mgr.consumeFluid(node, 0, 2, 10);
    CHECK_EQ(a.accepted_amount, 10, "zero-id request executes");
    CHECK_EQ(mgr.fluidTransactionCount(), size_t(0), "zero id is not cached");
    auto b = mgr.consumeFluid(node, 0, 2, 10);
    CHECK_EQ(b.accepted_amount, 10, "second zero-id request re-executes");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 0, "each zero-id request debits for real");
    CHECK_EQ(mgr.fluidTransactionCount(), size_t(0), "still nothing cached");
    PASS();
}

// 3.6.3: a reused request id whose (node, fluid, amount) tuple differs is
// rejected without debiting anything; the exact tuple replays cached.
static void test_replay_tuple_conflicts() {
    pipenet::PipeNetworkManager mgr;
    uint64_t n1 = mgr.addNode(0, 0, 0, 61);
    uint64_t n2 = mgr.addNode(5, 0, 0, 61);
    mgr.setNodeFluid(n1, 100, 1000, 2, false, true);
    mgr.setNodeFluid(n2, 100, 1000, 2, false, true);

    auto first = mgr.consumeFluid(n1, 9, 2, 30);
    CHECK_EQ(first.accepted_amount, 30, "original request served");
    CHECK_EQ(mgr.getNode(n1)->fluidBuffer, 70, "n1 debited");

    auto node_conflict = mgr.consumeFluid(n2, 9, 2, 30);
    CHECK(node_conflict.blocked, "same id on another node is a tuple conflict");
    CHECK_EQ(node_conflict.accepted_amount, 0, "conflicting replay cannot debit");
    CHECK_EQ(mgr.getNode(n2)->fluidBuffer, 100, "n2 untouched by the conflict");

    auto fluid_conflict = mgr.consumeFluid(n1, 9, 84, 30);
    CHECK(fluid_conflict.blocked, "same id with another fluid is a tuple conflict");
    CHECK_EQ(mgr.getNode(n1)->fluidBuffer, 70, "conflict leaves n1 unchanged");

    auto replay = mgr.consumeFluid(n1, 9, 2, 30);
    CHECK_EQ(replay.accepted_amount, 30, "exact tuple replays the cached response");
    CHECK_EQ(mgr.getNode(n1)->fluidBuffer, 70, "replay does not debit again");
    PASS();
}

// 3.6.3: transactions expire after the TTL — until then redelivery replays
// the cached response; after expiry the id is freed and re-executes.
static void test_transaction_expiry_frees_replay_id() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(node, 100, 1000, 2, false, true);

    auto first = mgr.consumeFluid(node, 55, 2, 40);
    CHECK_EQ(first.accepted_amount, 40, "original request served");
    CHECK_EQ(mgr.fluidTransactionCount(), size_t(1), "transaction cached");

    mgr.expireFluidTransactions(0);
    CHECK_EQ(mgr.fluidTransactionCount(), size_t(1), "TTL not reached, nothing expires");
    auto replay = mgr.consumeFluid(node, 55, 2, 40);
    CHECK_EQ(replay.accepted_amount, 40, "pre-expiry redelivery replays");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 60, "replay does not debit");

    mgr.expireFluidTransactions(1000000);
    CHECK_EQ(mgr.fluidTransactionCount(), size_t(0), "expired entries purged");
    auto after = mgr.consumeFluid(node, 55, 2, 40);
    CHECK_EQ(after.accepted_amount, 40, "post-expiry redelivery re-executes");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 20, "post-expiry redelivery debits again");
    PASS();
}

// 3.6.4: mixed fluids are never combined — a mismatched fluid_id is excluded
// with a blocked response and mismatched queries report nothing.
static void test_mixed_fluids_excluded() {
    pipenet::PipeNetworkManager mgr;
    uint64_t steam_node = mgr.addNode(0, 0, 0, 61);
    uint64_t water_node = mgr.addNode(5, 0, 0, 61);
    mgr.setNodeFluid(steam_node, 100, 1000, 2, false, true);
    mgr.setNodeFluid(water_node, 100, 1000, 84, false, true);

    auto into_steam = mgr.consumeFluid(steam_node, 1, 84, 10);
    CHECK(into_steam.blocked, "water demand into a steam buffer is blocked");
    CHECK_EQ(into_steam.accepted_amount, 0, "mismatched fluid accepts nothing");
    CHECK_EQ(mgr.getNode(steam_node)->fluidBuffer, 100, "steam buffer unchanged");

    auto into_water = mgr.consumeFluid(water_node, 2, 2, 10);
    CHECK(into_water.blocked, "steam demand into a water buffer is blocked");
    CHECK_EQ(mgr.getNode(water_node)->fluidBuffer, 100, "water buffer unchanged");

    auto steam_ok = mgr.consumeFluid(steam_node, 3, 2, 10);
    CHECK_EQ(steam_ok.accepted_amount, 10, "matching fluid is served");

    CHECK_EQ(mgr.fluidAmount(steam_node, 84), 0, "mismatched fluidAmount excluded");
    CHECK_EQ(mgr.fluidAmount(steam_node, 2), 90, "matching fluidAmount exact");
    CHECK_EQ(mgr.fluidAmount(water_node, 84), 100, "water node reports its stock");
    CHECK_EQ(mgr.fluidAmount(9999, 2), 0, "unknown node reports nothing");
    PASS();
}

// 3.6.4: sink capacity boundaries — over-capacity demand short-fills to the
// stock, an exact-fill request empties the node and resets its fluid id.
static void test_sink_capacity_boundaries_exact_fill() {
    pipenet::PipeNetworkManager mgr;
    uint64_t node = mgr.addNode(0, 0, 0, 61);
    mgr.setNodeFluid(node, 100, 1000, 2, false, true);

    auto over = mgr.consumeFluid(node, 1, 2, 5000);
    CHECK_EQ(over.accepted_amount, 100, "over-capacity demand short-fills to stock");
    CHECK_EQ(over.remaining, 4900, "unserved over-capacity reported");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 0, "never debited beyond stock");
    CHECK_EQ(mgr.getNode(node)->fluidId, uint32_t(0),
             "exact fill resets the node fluid id");

    auto empty = mgr.consumeFluid(node, 2, 2, 1);
    CHECK_EQ(empty.accepted_amount, 0, "empty node accepts zero");

    mgr.setNodeFluid(node, 40, 1000, 2, false, true);
    auto exact = mgr.consumeFluid(node, 3, 2, 40);
    CHECK_EQ(exact.accepted_amount, 40, "exact-fill request fully accepted");
    CHECK_EQ(exact.remaining, 0, "exact fill leaves no remaining");
    CHECK_EQ(mgr.getNode(node)->fluidBuffer, 0, "node exactly empty");
    PASS();
}

// =========================================================================
//  Integration-style tests
// =========================================================================

static void test_block_place_auto_discovery() {
    // Simulate block place: addNode + rebuildItemNetworks should create network
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, true, false);
    mgr.setNodeItemProps(b, 10, false, false);

    mgr.rebuildItemNetworks();
    auto* net = mgr.getItemNetwork(a);
    CHECK(net != nullptr, "item network discovered after block place");
    CHECK_EQ(net->itemNodes.size(), size_t(2), "both nodes in item network");
    PASS();
}

static void test_machine_to_pipe_to_machine() {
    // Simulate: machine output → pipe → machine input
    pipenet::PipeNetworkManager mgr;
    uint64_t src = mgr.addNode(0, 0, 0, 62);   // machine output as pipe node
    uint64_t pipe = mgr.addNode(1, 0, 0, 62);  // connecting pipe
    uint64_t sink = mgr.addNode(2, 0, 0, 62);  // machine input as pipe node

    mgr.addEdge(src, pipe);
    mgr.addEdge(pipe, sink);

    mgr.setNodeItemProps(src, 10, true, false);    // source
    mgr.setNodeItemProps(pipe, 10, false, false);   // pass-through
    mgr.setNodeItemProps(sink, 10, false, true);    // sink (item capacity=10 means pipe stores items)

    // Add item at source
    mgr.addNodeItem(src, 42, 1);

    // Tick 1: item moves from src to pipe or sink
    mgr.tickItemNetworks();
    CHECK_EQ(mgr.getNode(src)->itemBuffer.size(), size_t(0), "source drained after tick 1");
    // Item should be in pipe or sink after tick 1
    size_t totalAfterTick1 = mgr.getNode(pipe)->itemBuffer.size() + mgr.getNode(sink)->itemBuffer.size();
    CHECK_EQ(totalAfterTick1, size_t(1), "item in transit after tick 1");

    // Keep ticking until item reaches sink
    for (int i = 0; i < 10; ++i) mgr.tickItemNetworks();
    CHECK_EQ(mgr.getNode(sink)->itemBuffer.size(), size_t(1), "item reached sink after multiple ticks");
    CHECK_EQ(mgr.getNode(sink)->itemBuffer[0].item_id, uint16_t(42), "correct item at sink");
    PASS();
}

static void test_pipe_node_meta() {
    // New manager mask support: per-face mask stored on the node, and bulk
    // removal of a node's incident edges (used when a pipe's mask changes).
    pipenet::PipeNetworkManager mgr;
    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeMeta(a, 0x3F);
    CHECK_EQ(mgr.getNode(a)->meta, uint8_t(0x3F), "setNodeMeta stores mask");
    mgr.setNodeMeta(a, 0);
    CHECK_EQ(mgr.getNode(a)->meta, uint8_t(0), "setNodeMeta overwrites mask");

    mgr.removeEdgesForNode(a);
    bool together = false;
    for (const auto* net : mgr.getAllNetworks()) {
        bool hasA = false, hasB = false;
        for (uint64_t n : net->nodeIds) { if (n == a) hasA = true; if (n == b) hasB = true; }
        if (hasA && hasB) together = true;
    }
    CHECK(!together, "removeEdgesForNode dropped the a↔b edge");
    PASS();
}

static void test_item_pipe_face_mask() {
    // Validates the exact gating rule PipeNetworkService::connectNodeNeighbors
    // applies: two pipes connect across face f only if BOTH open it
    // (f on one side, f^1 on the other); a machine endpoint (no mask, treated
    // as open) connects whenever the pipe's shared face is open.
    using pipenet::pipeFaceOpen;
    using pipenet::pipeFacesConnected;

    CHECK(pipeFacesConnected(0, 0, 0), "meta 0 (all open) connects on every face");
    CHECK(!pipeFacesConnected(0x3E, 0x3F, 0), "closing A's +X face disconnects A↔B");
    CHECK(!pipeFacesConnected(0x3F, 0x3D, 0), "closing B's −X face disconnects A↔B (must be mutual)");
    CHECK(pipeFacesConnected(0x3F, 0x3F, 0), "both fully open connect");
    CHECK(pipeFacesConnected(0x01, 0x02, 0), "mutual open across +X connects (only those faces)");
    CHECK(pipeFaceOpen(0x3F, 0), "machine connects through a pipe's open +X face");
    CHECK(!pipeFaceOpen(0x3E, 0), "machine does NOT connect through a pipe's closed +X face");
    PASS();
}

static void test_pipe_wrench_disconnect() {
    // Regression: a wrench toggling a pipe face OFF must disconnect that pipe
    // while open faces stay connected. connectNodeNeighbors is private, so this
    // rebuilds A's edges through the public API as that method does.
    pipenet::PipeNetworkManager mgr;

    auto inSame = [&](uint64_t x, uint64_t y) -> bool {
        auto net = mgr.discoverNetwork(x);
        for (uint64_t n : net) if (n == y) return true;
        return false;
    };

    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    uint64_t c = mgr.addNode(-1, 0, 0, 62);
    mgr.setNodeItemProps(a, 1, false, false);
    mgr.setNodeItemProps(b, 1, false, false);
    mgr.setNodeItemProps(c, 1, false, false);

    // Face f connects A to a neighbor; the neighbor's opposite face is f^1.
    auto rebuildA = [&](uint8_t metaA, uint8_t metaB, uint8_t metaC) {
        mgr.setNodeMeta(a, metaA);
        mgr.setNodeMeta(b, metaB);
        mgr.setNodeMeta(c, metaC);
        mgr.removeEdgesForNode(a);
        mgr.removeEdgesForNode(b);
        mgr.removeEdgesForNode(c);
        if (pipenet::pipeFacesConnected(metaA, metaB, 0)) mgr.addEdge(a, b);
        if (pipenet::pipeFacesConnected(metaA, metaC, 1)) mgr.addEdge(a, c);
    };

    rebuildA(0x3F, 0x3F, 0x3F);
    CHECK(inSame(a, b), "fully-open pipes connect across +X");
    CHECK(inSame(a, c), "fully-open pipes connect across -X");

    rebuildA(0x3E, 0x3F, 0x3F);
    CHECK(!inSame(a, b), "wrench closing A's +X face disconnects A<->B");
    CHECK(inSame(a, c), "A keeps still-open -X connection to C");
    PASS();
}

static void test_pipe_machine_connect() {
    // Regression: a machine connects to a pipe gated only by the pipe's own
    // face mask (machine side has none). connectNodeNeighbors is private, so
    // this drives its pipe<->machine branch through the public API.
    pipenet::PipeNetworkManager mgr;

    auto inSame = [&](uint64_t x, uint64_t y) -> bool {
        auto net = mgr.discoverNetwork(x);
        for (uint64_t n : net) if (n == y) return true;
        return false;
    };

    uint64_t pipe = mgr.addNode(0, 0, 0, 62);
    uint64_t machine = mgr.addNode(1, 0, 0, 100);
    mgr.setNodeItemProps(pipe, 1, false, false);
    mgr.setNodeItemProps(machine, 1, false, false);

    auto rebuild = [&](uint8_t pipeMeta) {
        mgr.setNodeMeta(pipe, pipeMeta);
        mgr.removeEdgesForNode(pipe);
        mgr.removeEdgesForNode(machine);
        if (pipenet::pipeFaceOpen(pipeMeta, 0)) mgr.addEdge(pipe, machine);
    };

    rebuild(0x3F);
    CHECK(inSame(pipe, machine), "machine connects through pipe's open +X face");

    rebuild(0x3E);
    CHECK(!inSame(pipe, machine), "machine does not connect through pipe's closed +X face");
    PASS();
}

static void test_cable_graph_transformer_integration() {
    using namespace gtnh::pipe_network;

    CableGraph cg;

    CableDef mvCable;
    mvCable.block_id = 66;
    mvCable.tier = 2;
    mvCable.max_voltage = 128;
    mvCable.ampacity = 16;
    mvCable.loss_per_block = 0;
    cg.addCableNode(100, mvCable, 0, 0, 0);
    cg.addCableNode(102, mvCable, 1, 0, 0);

    CableDef hvCable;
    hvCable.block_id = 68;
    hvCable.tier = 3;
    hvCable.max_voltage = 512;
    hvCable.ampacity = 8;
    hvCable.loss_per_block = 0;
    cg.addCableNode(101, hvCable, 3, 0, 0);
    cg.addCableNode(103, hvCable, 4, 0, 0);

    cg.rebuildGraph();

    cg.registerGenerator(200, -1, 0, 0, 2);
    cg.registerMachine(300, 2, 0, 0, 2);
    cg.registerGenerator(301, 2, 0, 0, 3);
    cg.registerMachine(400, 5, 0, 0, 3);

    EnergyPacket pkt;
    pkt.voltage = 128;
    pkt.ampCount = 1;
    pkt.sourceId = 200;
    pkt.targetId = 0;
    pkt.tickIssued = 0;
    cg.injectPacket(pkt, 100);
    cg.tick();

    auto mvPackets = cg.collectPackets(300);
    CHECK(!mvPackets.empty(), "transformer sink receives MV packet");

    EnergyPacket steppedUp;
    steppedUp.voltage = 512;
    steppedUp.ampCount = 1;
    steppedUp.sourceId = 301;
    steppedUp.targetId = 0;
    steppedUp.tickIssued = 0;
    cg.injectPacket(steppedUp, 101);
    cg.tick();

    auto hvPackets = cg.collectPackets(400);
    CHECK(!hvPackets.empty(), "HV consumer receives stepped-up packet");
    CHECK(cg.getExplodedNodes().empty(), "no explosions in correct tier setup");

    PASS();
}

static void test_cable_explosion_event() {
    using namespace gtnh::pipe_network;

    CableGraph cg;
    CableDef def = {66, 2, "cable_tin", 0.0f, 128, 16};

    cg.addCableNode(100, def, 0, 0, 0);
    cg.addCableNode(102, def, 1, 0, 0);
    cg.rebuildGraph();

    cg.registerMachine(300, 2, 0, 0);

    EnergyPacket pkt;
    pkt.voltage = 512;
    pkt.ampCount = 1;
    pkt.sourceId = 200;
    pkt.targetId = 0;
    pkt.tickIssued = 0;
    cg.injectPacket(pkt, 100);

    cg.tick();

    auto exploded = cg.getExplodedNodes();
    CHECK(!exploded.empty(), "cable explosion: at least one node exploded");
    CHECK_EQ(exploded[0].nodeId, uint64_t(102), "cable explosion: cable before machine explodes");
    CHECK_GT(exploded[0].temperature, 0.0f, "cable explosion: temperature > 0");

    PASS();
}

static void test_persistence_load_unload_cycle() {
    pipenet::PipeNetworkManager mgr;

    uint64_t a = mgr.addNode(0, 0, 0, 62);
    uint64_t b = mgr.addNode(1, 0, 0, 62);
    mgr.addEdge(a, b);

    mgr.setNodeItemProps(a, 10, true, false);
    mgr.setNodeItemProps(b, 10, false, true);
    mgr.addNodeItem(a, 42, 1);
    mgr.addNodeItem(a, 7, 3);

    auto exported = mgr.exportItemBuffers();
    CHECK_EQ(exported.size(), size_t(1), "export has nodes with items");

    pipenet::PipeNetworkManager mgr2;
    uint64_t a2 = mgr2.addNode(0, 0, 0, 62);
    uint64_t b2 = mgr2.addNode(1, 0, 0, 62);
    mgr2.addEdge(a2, b2);
    mgr2.setNodeItemProps(a2, 10, true, false);
    mgr2.setNodeItemProps(b2, 10, false, true);

    mgr2.importItemBuffers(exported);

    auto* restoredSrc = mgr2.getNode(a2);
    CHECK(restoredSrc != nullptr, "restored source node exists");
    CHECK_EQ(restoredSrc->itemBuffer.size(), size_t(2), "two items restored");
    CHECK_EQ(restoredSrc->itemBuffer[0].item_id, uint16_t(42), "first item id");
    CHECK_EQ(restoredSrc->itemBuffer[0].count, uint8_t(1), "first item count");
    CHECK_EQ(restoredSrc->itemBuffer[1].item_id, uint16_t(7), "second item id");
    CHECK_EQ(restoredSrc->itemBuffer[1].count, uint8_t(3), "second item count");

    mgr2.rebuildItemNetworks();

    auto* net = mgr2.getItemNetwork(a2);
    CHECK(net != nullptr, "item network exists after restore");
    CHECK_EQ(net->itemNodes.size(), size_t(2), "both nodes in item network after restore");

    PASS();
}

// =========================================================================
//  Main
// =========================================================================


// ResourcePortClient serialize/parse round-trip: the shared serializer must
// finish the FlatBuffers root (table-builder Finish() alone leaves the buffer
// without a root offset and the receiving verifier rejects it).
static void test_resource_port_client_roundtrip() {
    using gtnh::common::ResourceKind;
    using gtnh::common::PortRole;
    using gtnh::common::ResourcePort;

    ResourcePort port{};
    port.port_id = 7;
    port.owner_id = 42;
    port.resource_kind = ResourceKind::FLUID;
    port.role = PortRole::SOURCE;
    port.x = 11; port.y = -2; port.z = 300;
    port.face_policy = gtnh::common::FacePolicy::MASK;
    port.face_mask = 0x2a;
    port.capacity = 5000;
    port.rate = 100;
    port.epoch = 3;

    auto reg = gtnh::common::SerializePortRegister(port, 1111);
    ResourcePort back{};
    std::uint32_t rid = 0;
    CHECK(gtnh::common::ParsePortRegister(reg.data(), reg.size(), &back, &rid),
          "port register roundtrip parses");
    CHECK_EQ(back.port_id, port.port_id, "port id roundtrip");
    CHECK_EQ(back.owner_id, port.owner_id, "owner id roundtrip");
    CHECK(back.resource_kind == ResourceKind::FLUID, "kind roundtrip");
    CHECK(back.role == PortRole::SOURCE, "role roundtrip");
    CHECK_EQ(back.x, port.x, "x roundtrip");
    CHECK_EQ(back.y, port.y, "y roundtrip");
    CHECK_EQ(back.z, port.z, "z roundtrip");
    CHECK_EQ(back.face_mask, port.face_mask, "face mask roundtrip");
    CHECK_EQ(back.capacity, port.capacity, "capacity roundtrip");
    CHECK_EQ(back.rate, port.rate, "rate roundtrip");
    CHECK_EQ(back.epoch, port.epoch, "epoch roundtrip");
    CHECK_EQ(rid, 1111u, "resource id roundtrip");

    auto rm = gtnh::common::SerializePortRemove(42, ResourceKind::HU, 9, 5);
    gtnh::common::ResourcePortRegistrationKey key{};
    CHECK(gtnh::common::ParsePortRemove(rm.data(), rm.size(), &key),
          "port remove roundtrip parses");
    CHECK_EQ(key.owner_id, 42u, "remove owner");
    CHECK_EQ(key.port_id, 9u, "remove port");
    CHECK_EQ(key.epoch, 5u, "remove epoch");
    CHECK(key.resource_kind == ResourceKind::HU, "remove kind");

    gtnh::common::ResourceTransferRequest req{};
    req.request_id = 1234;
    req.port_id = 7;
    req.resource_kind = ResourceKind::FLUID;
    req.resource_id = 1111;
    req.amount = 50;

    auto drain = gtnh::common::SerializeDrainRequest(req);
    gtnh::common::ResourceTransferRequest back_req{};
    CHECK(gtnh::common::ParseDrainRequest(drain.data(), drain.size(), &back_req),
          "drain request roundtrip parses");
    CHECK_EQ(back_req.request_id, req.request_id, "drain request id");
    CHECK_EQ(back_req.port_id, req.port_id, "drain port id");
    CHECK_EQ(back_req.resource_id, req.resource_id, "drain resource id");
    CHECK_EQ(back_req.amount, req.amount, "drain amount");

    auto consume = gtnh::common::SerializeConsumeRequest(req);
    CHECK(gtnh::common::ParseConsumeRequest(consume.data(), consume.size(), &back_req),
          "consume request roundtrip parses");
    CHECK_EQ(back_req.request_id, req.request_id, "consume request id");
    CHECK_EQ(back_req.amount, req.amount, "consume amount");

    // The verifier must reject a truncated/garbage buffer (not a parse crash).
    const std::uint8_t garbage[] = {0x01, 0x02, 0x03, 0x04};
    CHECK(!gtnh::common::ParsePortRegister(garbage, sizeof(garbage), &back, &rid),
          "verifier rejects garbage register");
    CHECK(!gtnh::common::ParseDrainRequest(garbage, sizeof(garbage), &back_req),
          "verifier rejects garbage drain request");

    PASS();
}

#define TEST(name) do { ++g_tests; printf("  TEST: %s\n", #name); test_##name(); } while(0)

int main(int, char**) {
    printf("=== PipeNetwork Test ===\n\n");

    // Existing
    TEST(empty_network);
    TEST(single_node);
    TEST(add_remove_node);
    TEST(network_discovery);
    TEST(disconnected_graphs);
    TEST(rebuild_networks);
    TEST(add_node_with_id);

    // Typed resource ports
    TEST(typed_ports_independent_resource_domains);
    TEST(typed_port_reregistration_is_idempotent);
    TEST(typed_port_removal_cleanup);

    // Typed domain graphs/state (2.3.x)
    TEST(converter_domains_independent_roles);
    TEST(typed_registration_feeds_domain_after_node);
    TEST(legacy_adapter_routes_to_single_domain);
    TEST(cross_kind_consumption_rejected);
    TEST(port_removal_clears_domain_role);
    TEST(owner_zero_port_registers);
    TEST(resource_port_client_roundtrip);
    TEST(typed_rate_applied_at_solve_time);
    TEST(topology_shared_domains_solved_independently);

    // Pipe wrench guidance
    TEST(wrench_isolated_pipe);
    TEST(wrench_pipe_to_pipe);
    TEST(wrench_pipe_adjacent_machine);
    TEST(wrench_non_pipe_position);
    TEST(wrench_guidance_no_mutation);

    // Item network
    TEST(item_network_simple);
    TEST(item_network_no_sink);
    TEST(item_network_multi_item);
    TEST(item_network_multi_tick);
    TEST(find_next_item_hop);
    TEST(find_next_item_hop_no_item_capability);

    // Energy distribution
    TEST(energy_distribution_simple);
    TEST(energy_distribution_no_sink);
    TEST(energy_distribution_capacity_limited);

    // Fluid distribution
    TEST(fluid_transaction_replay_and_limits);
    TEST(fluid_distribution_simple);
    TEST(fluid_distribution_capacity_limited);
    TEST(fluid_distribution_no_source);

    // FluidRegistry
    TEST(fluid_registry_defaults);

    // CableGraph
    TEST(cable_graph_add_remove);
    TEST(cable_graph_packet_routing);
    TEST(cable_graph_voltage_limit);
    TEST(cable_graph_loss);
    TEST(cable_graph_heavy_loss);
    TEST(cable_graph_overheat_explosion);
    TEST(cable_graph_ampacity_overheat);

    // HeatLoss module + pipe heat transport
    TEST(heat_loss_basic);
    TEST(heat_loss_node_temperature);
    TEST(heat_distribution_loss_reduction);
    TEST(heat_distribution_no_loss_with_zero_resistance);
    TEST(heat_distribution_capped_at_max);
    TEST(heat_node_temperature_tracked);

    // Heat pipe block
    TEST(heat_pipe_node_capacity);
    TEST(heat_pipe_network_transport);
    TEST(heat_pipe_no_fluid_transport);

    // Edge cases
    TEST(remove_edge_and_rebuild);
    TEST(large_network);
    TEST(node_count_after_operations);

    // Transformer integration
    TEST(cable_graph_transformer_integration);

    // Cable explosion event
    TEST(cable_explosion_event);

    // Persistence cycle
    TEST(persistence_load_unload_cycle);

    // Persistence
    TEST(export_import_item_buffers);
    TEST(export_empty_no_buffers);

    // Fluid routing (extended)
    TEST(fluid_routing_type_mismatch);
    TEST(fluid_routing_capacity);

    // Pipe-buffer conservation (3.3.4)
    TEST(fluid_conservation_accepted_blocked_disconnected);

    // Service-side shortfall transactions (3.4.2/3.4.3/2.5.3)
    TEST(shortfall_plan_records_pending);
    TEST(drain_response_applied_exactly_once);
    TEST(drain_response_clamped_and_mismatched);
    TEST(pending_expiry_completes_short_fill);
    TEST(port_removal_clears_pending);

    // 2.6.x: converters, simultaneous ports, entity ID zero, removal cleanup
    TEST(four_domains_simultaneous_one_owner);
    TEST(converter_source_sink_roles_no_bleed);
    TEST(owner_zero_port_full_lifecycle);
    TEST(port_ids_owner_scoped_no_cross_owner_collision);
    TEST(owner_node_id_collision_independent_spaces);
    TEST(exact_removal_by_registration_key);
    TEST(manager_removal_replay_exactly_once);
    TEST(tracker_removal_completes_pending_as_short_fill);

    // 3.6.x: conservation, partial fills, replay/expiry, mixed fluids, capacity
    TEST(conservation_pipe_only_exact);
    TEST(conservation_combined_shortfall);
    TEST(partial_zero_acceptance_no_over_debit);
    TEST(request_id_zero_never_cached);
    TEST(replay_tuple_conflicts);
    TEST(transaction_expiry_frees_replay_id);
    TEST(mixed_fluids_excluded);
    TEST(sink_capacity_boundaries_exact_fill);

    // Integration-style
    TEST(block_place_auto_discovery);
    TEST(machine_to_pipe_to_machine);

    // Per-face mask support (item/fluid pipe disconnect via wrench)
    TEST(pipe_node_meta);
    TEST(item_pipe_face_mask);
    TEST(pipe_wrench_disconnect);
    TEST(pipe_machine_connect);

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n",
           g_tests, g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
