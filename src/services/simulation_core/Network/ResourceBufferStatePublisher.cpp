#include "Network/ResourceBufferStatePublisher.h"

#include "Network/clients/IoUringRouterClient.h"
#include "common/ResourceBufferStateCodec.h"

#include <spdlog/spdlog.h>

namespace simcore {

ResourceBufferStatePublisher::ResourceBufferStatePublisher(
    std::shared_ptr<IoUringRouterClient> router)
    : router_(std::move(router)) {}

void ResourceBufferStatePublisher::PublishBufferState(
    std::uint64_t owner_id, gtnh::common::PortId port_id,
    gtnh::common::ResourceKind kind, std::uint32_t resource_id,
    std::int32_t amount, std::int32_t capacity, std::int32_t rate,
    std::uint64_t epoch, std::int32_t x, std::int32_t y, std::int32_t z) {
  if (!router_) return;

  std::uint64_t sequence = 0;
  {
    std::lock_guard<std::mutex> lock(sequence_mutex_);
    sequence = next_sequence_[{owner_id, port_id}]++;
  }

  gtnh::common::ResourceBufferStateMsg state;
  state.owner_id = owner_id;
  state.port_id = port_id;
  state.resource_kind = kind;
  state.resource_id = resource_id;
  state.amount = amount;
  state.capacity = capacity;
  state.rate = rate;
  state.epoch = epoch;
  state.sequence = sequence;
  state.x = x;
  state.y = y;
  state.z = z;
  state.removed = false;

  router_->Publish(gtnh::common::kTopicResourceBufferState,
                   gtnh::common::SerializeResourceBufferState(state));
  spdlog::debug("Published ResourceBufferState owner={} port={} kind={} id={} amount={}/{} seq={}",
                owner_id, port_id, static_cast<int>(kind), resource_id, amount,
                capacity, sequence);
}

}  // namespace simcore
