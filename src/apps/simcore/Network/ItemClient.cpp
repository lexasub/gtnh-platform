#include "Network/ItemClient.h"
#include "Network/clients/IoUringRouterClient.h"
#include "pipe_network_generated.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <vector>

namespace simcore {

ItemClient::ItemClient(std::shared_ptr<IoUringRouterClient> router)
    : router_(std::move(router))
{}

void ItemClient::publishNodeUpdate(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                                    const std::vector<uint16_t>& item_ids,
                                    const std::vector<uint8_t>& item_counts,
                                    int32_t capacity, bool is_source, bool is_sink,
                                    const std::vector<uint64_t>& connected_nodes)
{
    flatbuffers::FlatBufferBuilder builder(256);

    auto pos = Protocol::Vec3i(x, y, z);

    std::vector<Protocol::ItemStack> stacks;
    size_t count = std::min(item_ids.size(), item_counts.size());
    for (size_t i = 0; i < count; ++i) {
        if (item_ids[i] > 0 && item_counts[i] > 0) {
            stacks.push_back(Protocol::ItemStack(item_ids[i], item_counts[i], 0));
        }
    }

    auto update = Protocol::CreateItemNodeUpdateDirect(
        builder,
        node_id,
        &pos,
        stacks.empty() ? nullptr : &stacks,
        capacity,
        is_source,
        is_sink,
        connected_nodes.empty() ? nullptr : &connected_nodes
    );

    builder.Finish(update);
    std::vector<uint8_t> update_data(
        builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()
    );
    router_->Publish("item.node.update", update_data);
    spdlog::debug("Published ItemNodeUpdate: node_id={} at ({},{},{}) items={} cap={} src={} sink={}",
                  node_id, x, y, z, stacks.size(), capacity, is_source, is_sink);
}

void ItemClient::sendTransferRequest(uint64_t node_id, int32_t x, int32_t y, int32_t z,
                                      uint16_t item_id, uint8_t count, int32_t tier)
{
    flatbuffers::FlatBufferBuilder builder(64);

    auto pos = Protocol::Vec3i(x, y, z);

    auto req = Protocol::CreateItemTransferReq(
        builder,
        node_id,
        &pos,
        item_id,
        count,
        tier
    );

    builder.Finish(req);
    std::vector<uint8_t> req_data(
        builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()
    );
    router_->Publish("item.transfer.request", req_data);
    spdlog::debug("Published ItemTransferReq: node_id={} at ({},{},{}) item={} count={}",
                  node_id, x, y, z, item_id, count);
}

} // namespace simcore
