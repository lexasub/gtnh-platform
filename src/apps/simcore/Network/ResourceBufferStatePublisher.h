#pragma once

#include <common/ResourcePort.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace simcore {

class IoUringRouterClient;

// Publishes server-authoritative machine/port buffer snapshots to the
// client-facing gateway route (topic gtnh::common::kTopicResourceBufferState,
// payload Protocol::ResourceBufferState). This is the ONLY resource-state
// channel that reaches clients; internal PipeNetwork registration and
// drain/consume transaction topics stay service-internal.
//
// sequence is monotonically increasing per (owner_id, port_id) for the
// lifetime of this publisher; epoch is supplied by the caller (the port
// generation — 0 until typed port allocation lands, task 2.4.1).
class ResourceBufferStatePublisher {
 public:
  explicit ResourceBufferStatePublisher(
      std::shared_ptr<IoUringRouterClient> router);

  // Snapshot of one machine port buffer. amount/capacity/rate use the
  // channel's native unit (mB for FLUID). resource_id is the canonical packed
  // ItemId for FLUID/ITEM and 0 for energy channels.
  void PublishBufferState(std::uint64_t owner_id, gtnh::common::PortId port_id,
                          gtnh::common::ResourceKind kind,
                          std::uint32_t resource_id, std::int32_t amount,
                          std::int32_t capacity, std::int32_t rate,
                          std::uint64_t epoch, std::int32_t x, std::int32_t y,
                          std::int32_t z);

 private:
  std::shared_ptr<IoUringRouterClient> router_;
  std::mutex sequence_mutex_;
  std::map<std::pair<std::uint64_t, gtnh::common::PortId>, std::uint64_t>
      next_sequence_;
};

}  // namespace simcore
