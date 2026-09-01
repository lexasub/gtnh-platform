// Cross-service fixture for the client-facing ResourceBufferState channel
// (openspec refactor-fluid-port-accounting 5.4.2/5.4.3):
//   server-side serialization (same codec the SimulationCore publisher uses)
//   → gateway routing contract (verbatim pass-through + GatewayMsg type sync)
//   → client queue application → sequence ordering → removal clearing,
// plus fail-closed checks for malformed payloads, unknown ids, and mismatched
// epochs.
#include "Network/ResourceBufferStateStore.h"

#include <common/GatewayMsg.h>
#include <common/ItemId.h>
#include <common/ResourceBufferStateCodec.h>
#include <common/coords/Coords.h>

#include <Crafting/ClientItemRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

static int g_tests = 0, g_passed = 0, g_failed = 0;

static void test_check(bool cond, const char* file, int line, const char* expr,
                       const char* msg) {
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
#define PASS() do { ++g_passed; } while (0)

namespace {

gtnh::common::ResourceBufferStateMsg MakeState(uint64_t sequence,
                                               uint64_t epoch = 7) {
    gtnh::common::ResourceBufferStateMsg state;
    state.owner_id = 42;
    state.port_id = 1;
    state.resource_kind = gtnh::common::ResourceKind::FLUID;
    state.resource_id = ItemId::pack("1111:11:1");  // steam row in items.csv
    state.amount = 300;
    state.capacity = 1000;
    state.rate = 16;
    state.epoch = epoch;
    state.sequence = sequence;
    state.x = 10;
    state.y = 64;
    state.z = -20;
    state.removed = false;
    return state;
}

// Gateway routing is a verifying verbatim pass-through: the bytes the client
// enqueues are exactly the bytes the publisher serialized.
bool SimulateGatewayForward(const std::vector<uint8_t>& wire,
                            std::vector<uint8_t>* forwarded) {
    gtnh::common::ResourceBufferStateMsg check;
    if (!gtnh::common::ParseResourceBufferState(wire.data(), wire.size(),
                                                &check)) {
        return false;
    }
    *forwarded = wire;
    return true;
}

}  // namespace

static void test_gateway_msg_constant_sync() {
    // The client dispatch and the gateway forwarder must use the same wire
    // type for Protocol::ResourceBufferState. Both now include the shared
    // <common/GatewayMsg.h>; these asserts pin the value against accidental
    // renumbering.
    static_assert(GatewayMsg::kResourceBufferState == 47,
                  "ResourceBufferState wire type drifted");
    static_assert(GatewayMsg::kEntitySnap == GatewayMsg::kEntitySnapshot,
                  "client alias must track the shared constant");
    PASS();
}

static void test_client_topic_is_not_internal_pipe_topic() {
    // 5.1.3: the client-facing topic must never alias the internal
    // PipeNetwork registration/transaction topics. The internal names are
    // pinned here as literals on purpose: the client must not include the
    // transport-side headers that declare them.
    const std::string topic = gtnh::common::kTopicResourceBufferState;
    CHECK(topic != "resource.port.register",
          "client topic must differ from resource.port.register");
    CHECK(topic != "resource.port.remove",
          "client topic must differ from resource.port.remove");
    CHECK(topic != "resource.drain.request",
          "client topic must differ from resource.drain.request");
    CHECK(topic != "resource.drain.response",
          "client topic must differ from resource.drain.response");
    CHECK(topic != "resource.consume.request",
          "client topic must differ from resource.consume.request");
    CHECK(topic != "resource.consume.response",
          "client topic must differ from resource.consume.response");
    CHECK(topic != "fluid.node.update" && topic != "fluid.flow",
          "client topic must differ from legacy fluid transport topics");
    PASS();
}

static void test_publication_routing_and_application() {
    ResourceBufferStateStore store;
    auto wire = gtnh::common::SerializeResourceBufferState(MakeState(1));

    std::vector<uint8_t> forwarded;
    CHECK(SimulateGatewayForward(wire, &forwarded),
          "gateway forwards a valid ResourceBufferState verbatim");
    CHECK(forwarded == wire, "forwarded bytes are identical to published bytes");

    CHECK(store.Enqueue(std::make_shared<std::vector<uint8_t>>(forwarded)),
          "client enqueues the forwarded update");
    store.ApplyPending();

    const auto* entry = store.FindAt(BlockPos{10, 64, -20});
    CHECK(entry != nullptr, "applied update is visible at the machine position");
    if (entry) {
        CHECK(entry->owner_id == 42 && entry->port_id == 1,
              "owner/port identity round-trips");
        CHECK(entry->amount == 300 && entry->capacity == 1000,
              "amount/capacity round-trip");
        CHECK(entry->resource_id == ItemId::pack("1111:11:1"),
              "canonical resource id round-trips");
        CHECK(entry->epoch == 7 && entry->sequence == 1,
              "epoch/sequence round-trip");
    }
    PASS();
}

static void test_sequence_ordering() {
    ResourceBufferStateStore store;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(5))));
    store.ApplyPending();

    // Older sequence within the same epoch: discarded (out-of-order/replay).
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(3))));
    store.ApplyPending();
    auto* entry = store.FindAt(BlockPos{10, 64, -20});
    CHECK(entry != nullptr && entry->sequence == 5,
          "older sequence does not regress applied state");

    // Duplicate sequence: discarded.
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(5))));
    store.ApplyPending();
    entry = store.FindAt(BlockPos{10, 64, -20});
    CHECK(entry != nullptr && entry->amount == 300,
          "duplicate sequence is idempotent");

    // Newer sequence applies.
    auto newer = MakeState(6);
    newer.amount = 450;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(newer)));
    store.ApplyPending();
    entry = store.FindAt(BlockPos{10, 64, -20});
    CHECK(entry != nullptr && entry->sequence == 6 && entry->amount == 450,
          "newer sequence advances applied state");
    PASS();
}

static void test_epoch_gates_fail_closed() {
    ResourceBufferStateStore store;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(5, /*epoch=*/7))));
    store.ApplyPending();

    // Mismatched (older) epoch: discarded even with a higher sequence.
    auto stale = MakeState(9, /*epoch=*/6);
    stale.amount = 999;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(stale)));
    store.ApplyPending();
    auto* entry = store.FindAt(BlockPos{10, 64, -20});
    CHECK(entry != nullptr && entry->epoch == 7 && entry->amount == 300,
          "older epoch cannot corrupt displayed state");

    // Newer epoch starts a fresh generation: applies with any sequence.
    auto regenerated = MakeState(0, /*epoch=*/8);
    regenerated.amount = 100;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(regenerated)));
    store.ApplyPending();
    entry = store.FindAt(BlockPos{10, 64, -20});
    CHECK(entry != nullptr && entry->epoch == 8 && entry->amount == 100,
          "newer epoch re-baselines the generation");
    PASS();
}

static void test_removal_clearing_and_tombstones() {
    ResourceBufferStateStore store;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(5))));
    store.ApplyPending();
    CHECK(store.FindAt(BlockPos{10, 64, -20}) != nullptr,
          "entry exists before removal");

    // Removal clears the stored entry.
    auto removal = MakeState(6);
    removal.removed = true;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(removal)));
    store.ApplyPending();
    CHECK(store.FindAt(BlockPos{10, 64, -20}) == nullptr,
          "removed-port update clears stored state");

    // Updates for the removed port at the same/older epoch stay discarded.
    auto after_removal = MakeState(9);
    after_removal.amount = 777;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(after_removal)));
    store.ApplyPending();
    CHECK(store.FindAt(BlockPos{10, 64, -20}) == nullptr,
          "removed-port updates are discarded (tombstone)");

    // A higher epoch re-registers the port.
    auto reregistered = MakeState(0, /*epoch=*/8);
    reregistered.amount = 55;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(reregistered)));
    store.ApplyPending();
    auto* entry = store.FindAt(BlockPos{10, 64, -20});
    CHECK(entry != nullptr && entry->amount == 55 && entry->epoch == 8,
          "higher epoch re-registers a removed port");
    PASS();
}

static void test_malformed_payloads_fail_closed() {
    ResourceBufferStateStore store;
    auto good = std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(1)));
    CHECK(store.Enqueue(good), "sanity: valid payload enqueues");

    auto truncated = std::make_shared<std::vector<uint8_t>>(
        good->begin(), good->begin() + good->size() / 2);
    CHECK(!store.Enqueue(truncated), "truncated payload is rejected");

    auto garbage = std::make_shared<std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, 0x13, 0x37});
    CHECK(!store.Enqueue(garbage), "garbage payload is rejected");

    CHECK(!store.Enqueue(nullptr), "null payload is rejected");

    // Negative amounts are rejected by the codec (fail closed).
    gtnh::common::ResourceBufferStateMsg negative = MakeState(2);
    negative.amount = -5;
    auto negative_wire = std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(negative));
    CHECK(!store.Enqueue(negative_wire), "negative amount is rejected");

    store.ApplyPending();
    CHECK(store.size() == 1, "only the valid update reached the store");
    PASS();
}

static void test_chunk_and_reconnect_clearing() {
    ResourceBufferStateStore store;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(1))));
    auto far = MakeState(1);
    far.owner_id = 43;
    far.x = 1000;
    far.y = 64;
    far.z = 1000;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(far)));
    store.ApplyPending();
    CHECK(store.size() == 2, "two machines tracked");

    // Chunk eviction clears only that chunk's machines. (10,64,-20) lives in
    // chunk (0,2,-1); (1000,64,1000) in chunk (31,2,31).
    store.ClearChunk(ChunkCoord{0, 2, -1});
    CHECK(store.FindAt(BlockPos{10, 64, -20}) == nullptr,
          "evicted chunk state cleared");
    CHECK(store.FindAt(BlockPos{1000, 64, 1000}) != nullptr,
          "other chunk state survives");

    // Reconnect clears everything, including removal tombstones.
    auto removal = MakeState(2);
    removal.removed = true;
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(removal)));
    store.ApplyPending();
    store.Clear();
    CHECK(store.size() == 0, "reconnect clear empties the store");
    store.Enqueue(std::make_shared<std::vector<uint8_t>>(
        gtnh::common::SerializeResourceBufferState(MakeState(1))));
    store.ApplyPending();
    CHECK(store.FindAt(BlockPos{10, 64, -20}) != nullptr,
          "post-reconnect update applies fresh");
    PASS();
}

static void test_registry_label_resolution() {
    ItemRegistry::LoadFromCSV(DATA_DIR "/registry/items.csv");

    // Known canonical id resolves to the items.csv name (5.4.1).
    const uint16_t steam = ItemId::pack("1111:11:1");
    CHECK(ItemRegistry::GetName(steam) == "steam",
          "steam label resolves from items.csv");

    // Unknown id falls back to the registry placeholder without corrupting
    // the display (5.4.3).
    CHECK(ItemRegistry::GetName(0x1234) == "???",
          "unknown id yields the registry fallback label");
    PASS();
}

int main() {
    test_gateway_msg_constant_sync();
    test_client_topic_is_not_internal_pipe_topic();
    test_publication_routing_and_application();
    test_sequence_ordering();
    test_epoch_gates_fail_closed();
    test_removal_clearing_and_tombstones();
    test_malformed_payloads_fail_closed();
    test_chunk_and_reconnect_clearing();
    test_registry_label_resolution();

    fprintf(stderr, "%d/%d checks passed\n", g_passed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
