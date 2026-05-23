#include "Network/FluidClient.h"
#include "Network/clients/IoUringRouterClient.h"
#include "pipe_network_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <vector>

namespace simcore {

FluidClient::FluidClient(std::shared_ptr<IoUringRouterClient> router)
    : router_(std::move(router))
{}

void FluidClient::publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                                    uint32_t fluid_id, int32_t amount, int32_t capacity,
                                    int32_t max_input, int32_t max_output, int32_t tier,
                                    bool is_source, bool is_sink,
                                    const std::vector<uint64_t>& connected_nodes)
{
    flatbuffers::FlatBufferBuilder builder(256);

    auto pos = Protocol::Vec3i(x, y, z);

    auto update = Protocol::CreateFluidNodeUpdateDirect(
        builder,
        node_id,
        &pos,
        fluid_id,
        amount,
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
    router_->Publish("fluid.node.update", update_data);
    spdlog::debug("Published FluidNodeUpdate: node_id={} at ({},{},{}) fluid={} amount={}/{}",
                  node_id, x, y, z, fluid_id, amount, capacity);
}

void FluidClient::sendFluidRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                                   uint32_t fluid_id, int32_t amount)
{
    flatbuffers::FlatBufferBuilder builder(64);

    auto pos = Protocol::Vec3i(x, y, z);

    auto req = Protocol::CreateFluidConsumeReq(
        builder,
        node_id,
        &pos,
        fluid_id,
        amount
    );

    builder.Finish(req);
    std::vector<uint8_t> req_data(
        builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()
    );
    router_->Publish("fluid.consume.request", req_data);
    spdlog::debug("Published FluidConsumeReq: node_id={} at ({},{},{}) fluid_id={} amount={}",
                  node_id, x, y, z, fluid_id, amount);
}

} // namespace simcore
