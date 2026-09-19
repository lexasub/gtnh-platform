#include "Network/PipeEnergyClient.h"
#include "Network/clients/IoUringRouterClient.h"
#include "pipe_network_generated.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <vector>

namespace simcore {

PipeEnergyClient::PipeEnergyClient(std::shared_ptr<IoUringRouterClient> router)
    : router_(std::move(router))
{}

void PipeEnergyClient::publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                                        int32_t energy, int32_t capacity, int32_t max_input,
                                        int32_t max_output, int32_t tier, int32_t energy_type,
                                        bool is_source, bool is_sink,
                                        const std::vector<uint64_t>& connected_nodes)
{
    flatbuffers::FlatBufferBuilder builder(256);

    auto pos = Protocol::Vec3i(x, y, z);
    auto energy_type_enum = static_cast<Protocol::EnergyType>(energy_type);

    auto update = Protocol::CreateEnergyNodeUpdateDirect(
        builder,
        node_id,
        &pos,
        energy_type_enum,
        energy,
        capacity,
        max_input,
        max_output,
        tier,
        is_source,
        is_sink,
        connected_nodes.empty() ? nullptr : &connected_nodes
    );

    builder.Finish(update);
    std::vector<uint8_t> update_data(
        builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()
    );
    router_->Publish("energy.node.update", update_data);
    spdlog::debug("Published EnergyNodeUpdate: node_id={} at ({},{},{}) energy={}/{}, "
                  "max_in={}/max_out={}, tier={}, source={}, sink={}, connected_count={}",
                  node_id, x, y, z, energy, capacity, max_input, max_output, tier,
                  is_source, is_sink, connected_nodes.size());
}

void PipeEnergyClient::publishHeartbeat(uint64_t node_id, int32_t energy, int32_t capacity)
{
    flatbuffers::FlatBufferBuilder builder(64);

    auto pos = Protocol::Vec3i(0, 0, 0);
    auto energy_type_enum = Protocol::EnergyType_ELECTRICITY;

    auto update = Protocol::CreateEnergyNodeUpdateDirect(
        builder,
        node_id,
        &pos,
        energy_type_enum,
        energy,
        capacity,
        0,      // max_input
        0,      // max_output
        0,      // tier
        false,  // is_source
        false,  // is_sink
        nullptr // connected_nodes
    );

    builder.Finish(update);
    std::vector<uint8_t> update_data(
        builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()
    );
    router_->Publish("energy.node.update", update_data);
    spdlog::debug("Published EnergyNodeUpdate heartbeat: node_id={} energy={}/{}",
                  node_id, energy, capacity);
}

void PipeEnergyClient::sendConsumeRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                                          int32_t energy_type, int32_t amount)
{
    flatbuffers::FlatBufferBuilder builder(64);

    auto pos = Protocol::Vec3i(x, y, z);
    auto energy_type_enum = static_cast<Protocol::EnergyType>(energy_type);

    auto req = Protocol::CreateEnergyConsumeReq(
        builder,
        node_id,
        &pos,
        energy_type_enum,
        amount
    );

    builder.Finish(req);
    std::vector<uint8_t> req_data(
        builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()
    );
    router_->Publish("energy.consume.request", req_data);
    spdlog::debug("Published EnergyConsumeReq: node_id={} at ({},{},{}) energy_type={} amount={}",
                  node_id, x, y, z, energy_type, amount);
}

} // namespace simcore
