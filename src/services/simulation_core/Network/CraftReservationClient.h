#pragma once

// Client-side craft resource orchestration (openspec
// refactor-fluid-port-accounting, tasks 4.2.2/4.2.4/4.3.1/4.3.3/4.3.4).
//
// SimulationCore owns the PendingCraft lifecycle: before a recipe's input
// items are touched, every resource requirement is reserved through a typed
// ResourceConsumeRequest on "resource.consume.request"; responses arrive on
// "resource.consume.response" and are correlated by request ID and
// requirement — never FIFO. A zero/partial response leaves the craft pending
// with bounded retry/backoff; inputs are consumed and progress starts only
// after every reservation is fully accepted.
//
// The same request/response path charges recurring per-tick requirements of
// an ACTIVE recipe before its progress may advance (4.3.4): the accepted
// amount credits the machine's EnergyStorage, and the tick debit happens only
// when the buffer covers the requirement amounts.
//
// Single-threaded: all entry points run on the simcore main queue.
//
// Request serialization is local (two-stage FlatBuffers Finish) for the same
// reason ResourceDrainHandler serializes locally: the shared Serialize*
// helpers in ResourcePortClient.h leave the builder unfinished.

#include <common/ResourcePort.h>
#include <common/ResourcePortClient.h>

#include <entt/entt.hpp>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RecipeManager {
struct Recipe;
}

namespace simcore {

struct RecipeProgress;

class CraftReservationClient {
public:
  // Generic (topic, payload) publisher; wired to IoUringRouterClient::Publish
  // in main.cpp and to a capturing lambda in tests.
  using PublishFn = std::function<bool(const char* topic,
                                       const std::vector<std::uint8_t>& payload)>;

  CraftReservationClient(entt::registry& reg, PublishFn publish);

  // 4.2.2: build the machine's PendingCraft from the recipe requirements and
  // publish one typed consume request per requirement. Input inventory is not
  // touched here. Returns false when nothing was requested (no requirements,
  // no publisher, or a pending craft already exists).
  bool beginReservation(entt::entity entity,
                        const RecipeManager::Recipe& recipe,
                        std::uint64_t now_tick);

  // 4.3.3: bounded retry/backoff for a pending craft. Re-publishes the
  // not-yet-accepted requirements (for their remaining amounts) with fresh
  // request ids when the backoff deadline passed; cancels the craft when the
  // retry budget is exhausted or the expiry tick passed. Returns false when
  // the pending craft was cancelled.
  bool tickPending(entt::entity entity, std::uint64_t now_tick);

  // 4.3.1: correlate a typed response by request ID. Records acceptance on
  // the matching reservation (reservation leg) or credits the machine buffer
  // (per-tick charge leg) exactly once per request id. Returns the entity
  // whose pending craft became fully accepted and is ready to commit, or
  // entt::null.
  entt::entity onConsumeResponse(
      const gtnh::common::ResourceTransferResponse& response);

  // 4.2.4: drop every outstanding request of the machine (both legs) and
  // clear its pending craft. Safe for destroyed entities.
  void cancel(entt::entity entity, std::string_view reason);

  // 4.3.4: recurring per-tick charge for an active recipe. Publishes one
  // typed consume request for the recipe's total per-tick requirement
  // amount. Returns the request id, or 0 when a charge is already
  // outstanding or the recipe has no requirements.
  std::uint64_t beginPerTickCharge(entt::entity entity,
                                   const RecipeManager::Recipe& recipe);

  bool hasOutstandingCharge(entt::entity entity) const;

  static constexpr std::uint32_t kMaxRetries = 6;
  static constexpr std::uint64_t kRetryBackoffTicks = 10;
  static constexpr std::uint64_t kReservationExpiryTicks = 100;

private:
  // One published request awaiting its response. requirement_id 0 marks the
  // per-tick charge leg; reservation legs carry the 1-based requirement
  // index from PendingCraft.
  struct OutstandingRequest {
    entt::entity entity;
    std::uint32_t requirement_id = 0;
    std::int32_t requested = 0;
  };

  std::uint64_t mintRequestId();
  bool publishConsumeRequest(std::uint64_t request_id,
                             gtnh::common::ResourceKind kind,
                             std::uint32_t resource_id, std::int32_t amount);
  void creditBuffer(entt::entity entity, std::int32_t amount);

  entt::registry& reg_;
  PublishFn publish_;
  std::uint64_t next_request_id_ = 1;
  std::unordered_map<std::uint64_t, OutstandingRequest> outstanding_;
  // entity -> number of outstanding per-tick charge requests.
  std::unordered_map<entt::entity, std::size_t> charge_by_entity_;
};

} // namespace simcore
