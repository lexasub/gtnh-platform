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
// thread; no locking is required (and none is done). The conflict-warning
// suppression set below is part of that same self-owned state: it carries no
// mutex for the same reason ports_ does not.

#include <common/ResourcePort.h>
#include <common/ResourcePortClient.h>

#include <entt/entt.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
  // Upper bound on remembered (port, owner, kind) conflict identities. The set
  // is normally near-empty — it only gains an entry while a port is registered
  // and the entry is dropped the moment that port is deregistered, successfully
  // re-registered, or removed with its owner, so it tracks ports_ rather than
  // total history. The cap is a backstop for a hostile peer that keeps
  // colliding distinct identities on a single port: past the cap the oldest
  // entry is forgotten, which can re-enable at most one extra warning per
  // eviction instead of growing memory without bound.
  static constexpr std::size_t kConflictWarnMaxEntries = 1024;

private:
  struct ReplayEntry {
    gtnh::common::ResourceTransferResponse response;
    std::uint64_t owner_id = 0;
  };

  void cacheResponse(std::uint64_t request_id, const ReplayEntry& entry);
  void publishResponse(const gtnh::common::ResourceTransferResponse& response);
  // Warn once per conflicting (port_id, owner, kind) identity; repeats of an
  // already-warned identity are dropped. Self-owned state, so it inherits the
  // single-threaded contract of the rest of the handler.
  bool shouldWarnConflict(const gtnh::common::ResourcePort& incoming);
  // Forget every remembered conflict of `port_id` — called when the port leaves
  // the registry or a re-registration lands cleanly, so the next conflict on a
  // reused port_id is reported again.
  void forgetPortConflicts(gtnh::common::PortId port_id);
  // Forget every remembered conflict owned by `owner_id`.
  void forgetOwnerConflicts(std::uint64_t owner_id);
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
  // Conflicting re-registrations already reported, keyed by the wire
  // registration key of the *incoming* port. Two ports fighting over one
  // port_id (measured 205 warn/s) each land one entry and then go silent, so
  // the log volume is bounded by distinct identities, not by occurrences.
  // Insertion-ordered by conflict_warn_order_ so the cap can evict the oldest.
  std::unordered_set<gtnh::common::ResourcePortRegistrationKey> conflict_warned_;
  std::deque<gtnh::common::ResourcePortRegistrationKey> conflict_warn_order_;
};

} // namespace simcore
