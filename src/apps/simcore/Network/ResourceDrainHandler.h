#pragma once

// Owner-side typed resource drain endpoint (openspec
// refactor-fluid-port-accounting, tasks 3.2.1-3.2.4 + 2.5.2).
//
// SimulationCore owns machine buffers (SteamOutputComponent, FluidStorage);
// PipeNetwork owns only fluid physically stored in pipe nodes. When a
// ResourceDrainRequest arrives on "resource.drain.request", this handler is
// the transaction owner: it validates the request against the port registry,
// debits the machine buffer exactly once per request_id, and answers on
// "resource.drain.response".
//
// The port registry is fed by the typed "resource.port.register" /
// "resource.port.remove" topics (the same messages the machine registration
// path publishes for PipeNetwork), so this handler never mutates pipe-owned
// state and stays decoupled from the registration call sites.
//
// Single-threaded: handle*() runs on the simcore main queue and
// removeOwnerPorts() is invoked from SimulationEngine callbacks on the same
// thread; no locking is required (and none is done).

#include <common/ResourcePort.h>
#include <common/ResourcePortClient.h>

#include <entt/entt.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace simcore {

class ResourceDrainHandler {
public:
  // Generic (topic, payload) publisher; wired to IoUringRouterClient::Publish
  // in main.cpp and to a capturing lambda in tests.
  using PublishFn = std::function<bool(const char* topic,
                                       const std::vector<std::uint8_t>& payload)>;

  ResourceDrainHandler(entt::registry& reg, PublishFn publish,
                       std::uint16_t steam_item_id);

  // -- TopicDispatcher entry points (one object, three topics) --------------

  // "resource.port.register": learn ports owned by this service.
  void handlePortRegister(const std::vector<std::uint8_t>& data);
  // "resource.port.remove": exact (owner, kind, port, epoch) removal.
  void handlePortRemove(const std::vector<std::uint8_t>& data);
  // "resource.drain.request": the owner-side drain transaction.
  void handleDrainRequest(const std::vector<std::uint8_t>& data);

  // -- Owner destruction / replacement (task 2.5.2) --------------------------
  // Publishes ResourcePortRemove for every port of `owner_id`, drops them
  // from the registry, and purges that owner's replay-cache entries. Wired to
  // SimulationEngine::onMachineOwnerRemoved.
  void removeOwnerPorts(std::uint64_t owner_id);

  static constexpr std::size_t kReplayCacheMaxEntries = 4096;

private:
  struct ReplayEntry {
    gtnh::common::ResourceTransferResponse response;
    std::uint64_t owner_id = 0;
  };

  void cacheResponse(std::uint64_t request_id, const ReplayEntry& entry);
  void publishResponse(const gtnh::common::ResourceTransferResponse& response);
  // Drain against the machine buffer; returns accepted amount (>= 0).
  std::int32_t drainMachineBuffer(const gtnh::common::ResourcePort& port,
                                  const gtnh::common::ResourceTransferRequest& request);
  std::int32_t drainSteamOutput(entt::entity entity,
                                const gtnh::common::ResourceTransferRequest& request,
                                std::int32_t rate_limit);
  std::int32_t drainFluidStorage(entt::entity entity,
                                 const gtnh::common::ResourceTransferRequest& request,
                                 std::int32_t rate_limit);

  entt::registry& reg_;
  PublishFn publish_;
  // Registry-resolved Steam item id; 0 (registry unavailable) fails closed:
  // SteamOutputComponent buffers accept no drain.
  std::uint16_t steam_id_ = 0;
  std::unordered_map<gtnh::common::PortId, gtnh::common::ResourcePort> ports_;
  std::unordered_map<std::uint64_t, ReplayEntry> replay_;
  std::deque<std::uint64_t> replay_order_;
};

} // namespace simcore
