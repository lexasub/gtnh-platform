#pragma once

// Service-side fluid consume transactions (openspec
// refactor-fluid-port-accounting 3.4.2/3.4.3/2.5.3).
//
// PipeNetworkService consumes pipe buffers first (manager-owned, 3.3.1) and
// asks the source OWNER (SimulationCore) to drain exactly the shortfall via a
// typed ResourceDrainRequest. The owner's answer arrives asynchronously on
// "resource.drain.response" and must be applied EXACTLY ONCE to the waiting
// consume; responses correlate by request_id, never by FIFO or position
// (3.5.2). This header is the pure state machine for that pending set — no
// FlatBuffers, no router — so the semantics are unit-testable directly.
//
// Bounding: every pending entry carries an expiry tick; expire() erases past
// entries so the map is bounded by (shortfall request rate x TTL). The caller
// completes expired entries with their pipe-only amount (short-fill), so a
// consumer never hangs when an owner answer is lost.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <common/ResourcePort.h>

namespace gtnh {
namespace pipe_network {

// Pending-consume TTL in service ticks (tick = 100 ms, so 50 ticks = 5 s).
inline constexpr uint64_t kPendingConsumeTtlTicks = 50;

// One outstanding consume whose pipe-buffer part is already served and whose
// source shortfall waits for a ResourceDrainResponse.
struct PendingConsume {
  uint64_t drain_request_id = 0;            // key; correlates the response
  uint64_t consume_request_id = 0;          // service<->manager leg (diagnostics)
  uint64_t sink_node_id = 0;                // manager node the consumer addressed
  uint64_t source_node_id = 0;              // manager node of the resolved source
  uint64_t source_owner_id = 0;             // typed port owner (0 = node-based)
  gtnh::common::PortId source_port_id = 0;  // 0 = node-based fallback request
  uint32_t fluid_id = 0;                    // canonical packed fluid item id
  int32_t requested = 0;                    // original consume amount
  int32_t pipe_accepted = 0;                // already served from pipe buffers
  uint64_t expires_at = 0;                  // service tick deadline
};

// Outcome of applying a ResourceDrainResponse to a pending consume.
struct DrainApplyResult {
  bool applied = false;          // false: unknown id, replay, or fluid mismatch
  int32_t source_accepted = 0;   // clamped amount credited from the source
  int32_t combined_accepted = 0; // pipe part + source part
  int32_t remaining = 0;         // requested - combined
  PendingConsume completed;      // valid when applied
};

// What the service should emit after a pipe-first consume left a shortfall.
struct ShortfallDecision {
  bool request_source = false;   // emit a ResourceDrainRequest?
  uint64_t drain_request_id = 0;
  uint64_t source_node_id = 0;
  uint64_t source_owner_id = 0;
  gtnh::common::PortId source_port_id = 0;
  int32_t shortfall = 0;         // amount to drain (only the unserved part)
};

class PipeConsumeTracker {
 public:
  explicit PipeConsumeTracker(uint64_t ttl_ticks) : ttl_ticks_(ttl_ticks) {}

  // Records the pending consume for a shortfall the resolved source
  // (source_node_id != 0) must fill. Returns the emission decision; when it
  // is not request_source (no shortfall, no source, or zero request id) the
  // caller must answer the consumer immediately and nothing is recorded.
  ShortfallDecision planShortfall(uint64_t drain_request_id,
                                  uint64_t consume_request_id,
                                  uint64_t sink_node_id, uint32_t fluid_id,
                                  int32_t requested, int32_t pipe_accepted,
                                  uint64_t source_node_id,
                                  uint64_t source_owner_id,
                                  gtnh::common::PortId source_port_id,
                                  uint64_t now_tick) {
    ShortfallDecision decision;
    const int32_t shortfall = requested - pipe_accepted;
    if (shortfall <= 0 || source_node_id == 0 || drain_request_id == 0) {
      return decision;
    }

    decision.request_source = true;
    decision.drain_request_id = drain_request_id;
    decision.source_node_id = source_node_id;
    decision.source_owner_id = source_owner_id;
    decision.source_port_id = source_port_id;
    decision.shortfall = shortfall;

    PendingConsume pending;
    pending.drain_request_id = drain_request_id;
    pending.consume_request_id = consume_request_id;
    pending.sink_node_id = sink_node_id;
    pending.source_node_id = source_node_id;
    pending.source_owner_id = source_owner_id;
    pending.source_port_id = source_port_id;
    pending.fluid_id = fluid_id;
    pending.requested = requested;
    pending.pipe_accepted = pipe_accepted;
    pending.expires_at = now_tick + ttl_ticks_;
    pending_.emplace(drain_request_id, pending);
    return decision;
  }

  // Applies a drain response exactly once (replay guard keyed by request id:
  // applied entries are erased, so a repeated response finds nothing).
  // Responses for unknown/expired ids or a mismatched fluid id are ignored.
  // The accepted amount is clamped to [0, shortfall]: negative acceptance
  // completes the consume as blocked (zero source part), over-acceptance is
  // capped so combined never exceeds requested.
  DrainApplyResult applyDrainResponse(uint64_t drain_request_id,
                                      uint32_t fluid_id,
                                      int32_t accepted_amount) {
    DrainApplyResult result;
    auto it = pending_.find(drain_request_id);
    if (it == pending_.end()) return result;
    const PendingConsume pending = it->second;
    if (fluid_id != pending.fluid_id) return result;

    int32_t accepted = accepted_amount;
    if (accepted < 0) accepted = 0;
    const int32_t shortfall = pending.requested - pending.pipe_accepted;
    if (accepted > shortfall) accepted = shortfall;

    pending_.erase(it);
    result.applied = true;
    result.source_accepted = accepted;
    result.combined_accepted = pending.pipe_accepted + accepted;
    result.remaining = pending.requested - result.combined_accepted;
    result.completed = pending;
    return result;
  }

  // Erases pending consumes past their deadline and returns them; the caller
  // completes each with its pipe-only amount (short-fill).
  std::vector<PendingConsume> expire(uint64_t now_tick) {
    std::vector<PendingConsume> expired;
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.expires_at <= now_tick) {
        expired.push_back(it->second);
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
    return expired;
  }

  // Completes every pending consume addressed to the removed (owner, port)
  // pair (2.5.1/2.5.3). Node-based fallback pendings (port_id 0) are never
  // matched here — they are TTL-bound. Port ids are owner-scoped, so the
  // owner must match exactly.
  std::vector<PendingConsume> clearForPort(uint64_t owner_id,
                                           gtnh::common::PortId port_id) {
    std::vector<PendingConsume> cleared;
    if (port_id == 0) return cleared;
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.source_port_id == port_id &&
          it->second.source_owner_id == owner_id) {
        cleared.push_back(it->second);
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
    return cleared;
  }

  size_t size() const { return pending_.size(); }
  bool empty() const { return pending_.empty(); }

 private:
  uint64_t ttl_ticks_ = 0;
  std::unordered_map<uint64_t, PendingConsume> pending_;
};

} // namespace pipe_network
} // namespace gtnh
