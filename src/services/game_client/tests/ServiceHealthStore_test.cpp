#include "../Network/ServiceHealthStore.h"

#include <service_health_generated.h>
#include <cassert>
#include <flatbuffers/flatbuffers.h>

int main() {
    flatbuffers::FlatBufferBuilder builder;
    auto name = builder.CreateString("gateway");
    auto row = Protocol::CreateServiceHealth(
        builder, name, true, Protocol::ServiceHealthProbeState_UP, 12);
    auto rows = builder.CreateVector(&row, 1);
    auto response = Protocol::CreateServiceHealthResp(builder, 1, rows);
    builder.Finish(response);
    auto bytes = std::make_shared<std::vector<uint8_t>>(
        builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());

    ServiceHealthStore store;
    store.Apply(bytes);
    const auto snapshot = store.Snapshot();
    assert(snapshot.size() == 1);
    assert(snapshot.front().name == "gateway");
    assert(snapshot.front().transportAlive);
    assert(snapshot.front().probeState == Protocol::ServiceHealthProbeState_UP);
    assert(snapshot.front().lastResponseAgeMs == 12);
    return 0;
}
