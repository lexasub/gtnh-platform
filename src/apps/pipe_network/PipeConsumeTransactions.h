#pragma once

// Service-side fluid consume transactions (openspec
// refactor-fluid-port-accounting 3.4.2/3.4.3/2.5.3/3.5.3/3.5.4).
//
// PipeNetworkService consumes pipe buffers first (manager-owned, 3.3.1) and
// asks the source OWNER (SimulationCore) to drain exactly the shortfall via a
// typed ResourceDrainRequest. The owner's answer arrives asynchronously on
// "resource.drain.response" and must be applied EXACTLY ONCE to the waiting
// consume; responses correlate by request_id, never by FIFO or position
// (3.5.2). This header is the pure state machine for that pending set — no
// FlatBuffers, no router — so the semantics are unit-testable directly.
//
// Bounding (3.5.3): every pending entry carries an expiry tick; expire()
// erases past entries so the map is bounded by (shortfall request rate x TTL).
// The caller completes expired entries with their pipe-only amount
// (short-fill), so a consumer never hangs when an owner answer is lost. A
// pending whose owner answer is merely slow is re-published with doubling
// backoff up to a retry cap; a retry re-uses the SAME request id, which the
// owner's replay cache answers exactly once, so a retry can never double
// debit. At most one publish per pending is ever in flight.
//
// Cancellation (3.5.3): a pending whose source port is removed or re-registered
// under a new epoch (3.5.4) is MARKED cancelled and kept until its TTL so a
// late response is recognized and dropped by the exactly-once apply path
// instead of surfacing as an unknown id. Cancelled entries are never
// completed twice: cancel/expire/clearForPort each skip already-cancelled
// entries.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <common/ResourcePort.h>

namespace gtnh {
namespace pipe_network {

// --- 3.5.3 bounds (service tick = 100 ms) --------------------------------
//
// Overall deadline for a pending shortfall drain: 50 ticks = 5 s. Primary
// bound; a consumer waits at most 5 s for a lost owner response before it is
// completed short-fill (design.md failure handling: "Unknown/removed ports
// return a zero accepted amount and clear pending state" — expiry is the
// lost-response analogue).
inline constexpr uint64_t kPendingConsumeTtlTicks = 50;

// First retry fires 2 ticks (200 ms) after the initial publish: quick enough
// to cover a lost pub/sub message, slow enough not to hot-loop the owner.
inline constexpr uint64_t kDrainRetryBaseBackoffTicks = 2;

// Backoff doubles per retry (2, 4, 8, ... ticks between attempts)...
inline constexpr uint32_t kDrainRetryBackoffMultiplier = 2;

// ...and caps at 16 ticks (1.6 s) so the spacing stays bounded if the retry
// cap below is ever raised.
inline constexpr uint64_t kDrainRetryMaxBackoffTicks = 16;

// At most 4 re-publishes per pending. At doubling backoff the 4 attempts land
// at +2/+4/+8/+16 ticks, always inside the 50-tick TTL; the TTL, not this
// cap, is the primary bound. The cap only limits the retry count if the TTL
// is ever raised.
inline constexpr uint32_t kMaxDrainRetries = 4;

// Rejection cooldown (3.5.3): a source that answered a shortfall drain with
// zero acceptance cools down before the next FRESH shortfall is planned
// against it, so a failing (empty, mismatched, or half-removed) source is not
// hammered once per consumer poll. First rejection cools 5 ticks (0.5 s);
// consecutive rejections double the window (10, 20, 40) up to the 50-tick cap
// (5 s — the same scale as the pending TTL). A productive answer, a
// (re-)registration of the port, or cancellation of the node/port clears the
// cooldown. Unlike the retry schedule above (which re-publishes an in-flight
// request), this gates new planShortfall() calls.
inline constexpr uint64_t kSourceRejectBaseBackoffTicks = 5;
inline constexpr uint64_t kSourceRejectMaxBackoffTicks = 50;

// Cooldown after the Nth consecutive zero-accepted answer (N = 1-based):
// base doubled per repeat, capped at kSourceRejectMaxBackoffTicks.
inline uint64_t sourceRejectBackoffTicks(uint32_t consecutive) {
  uint64_t window = kSourceRejectBaseBackoffTicks;
  for (uint32_t i = 1; i < consecutive && window < kSourceRejectMaxBackoffTicks;
       ++i) {
    window *= 2;
  }
  return window > kSourceRejectMaxBackoffTicks ? kSourceRejectMaxBackoffTicks
                                               : window;
}

// Backoff scheduled AFTER the retry_count-th attempt (1-based): base doubled
// per attempt, capped at kDrainRetryMaxBackoffTicks. retry_count 0 (the
// initial delay before the first retry) uses the base backoff.
inline uint64_t drainRetryBackoffTicks(uint32_t retry_count) {
  uint64_t backoff = kDrainRetryBaseBackoffTicks;
  for (uint32_t i = 1; i < retry_count; ++i) {
    backoff *= kDrainRetryBackoffMultiplier;
    if (backoff >= kDrainRetryMaxBackoffTicks) return kDrainRetryMaxBackoffTicks;
  }
  return backoff;
}

// One outstanding consume whose pipe-buffer part is already served and whose
// source shortfall waits for a ResourceDrainResponse.
struct PendingConsume {
  uint64_t drain_request_id = 0;            // key; correlates the response
  uint64_t consume_request_id = 0;          // service<->manager leg (diagnostics)
  uint64_t sink_node_id = 0;                // manager node the consumer addressed
  uint64_t source_node_id = 0;              // manager node of the resolved source
  uint64_t source_owner_id = 0;             // typed port owner (0 = node-based)
  gtnh::common::PortId source_port_id = 0;  // 0 = node-based fallback request
  uint64_t source_epoch = 0;                // port epoch at plan time (3.5.4)
  uint32_t fluid_id = 0;                    // canonical packed fluid item id
  int32_t requested = 0;                    // original consume amount
  int32_t pipe_accepted = 0;                // already served from pipe buffers
  uint64_t expires_at = 0;                  // service tick deadline (TTL)
  uint32_t retry_count = 0;                 // re-publishes so far (3.5.3)
  uint64_t next_retry_at = 0;               // tick of the next allowed retry
  bool cancelled = false;                   // marked; late responses dropped
};

// Outcome of applying a ResourceDrainResponse to a pending consume.
struct DrainApplyResult {
  bool applied = false;          // false: unknown id, replay, cancel, mismatch
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

// One due re-publish of a shortfall drain request (3.5.3). Same request id as
// the original: the owner's replay cache answers re-delivery exactly once.
struct DrainRetry {
  uint64_t drain_request_id = 0;
  uint64_t source_node_id = 0;
  uint64_t source_owner_id = 0;
  gtnh::common::PortId source_port_id = 0;
  uint64_t source_epoch = 0;     // epoch the request was planned against
  uint32_t fluid_id = 0;
  int32_t shortfall = 0;
  uint32_t retry_count = 0;      // after increment (1-based)
};

// 3.5.4(a)/(c): may a drain response be applied given the live registered
// state of the pending's source port? Node-based fallback pendings (port 0)
// predate the typed contract and bypass the registry check; typed pendings
// require the port to still be registered at the epoch the request was
// planned against — a removed port or a re-registered (new-epoch) port
// invalidates the response.
inline bool drainResponsePortLive(const PendingConsume& pending,
                                  bool port_registered,
                                  uint64_t registered_epoch) {
  if (pending.source_port_id == 0) return true;
  return port_registered && registered_epoch == pending.source_epoch;
}

class PipeConsumeTracker {
 public:
  // Identifies a shortfall source for rejection cooldown: the manager node
  // plus the typed port identity (owner/port 0 for node-based fallback
  // requests). Port ids are owner-scoped; kind is implicit (pending drains
  // are FLUID-only).
  struct SourceKey {
    uint64_t node_id = 0;
    uint64_t owner_id = 0;
    gtnh::common::PortId port_id = 0;
    friend bool operator==(const SourceKey&, const SourceKey&) = default;
  };
  struct SourceKeyHash {
    size_t operator()(const SourceKey& key) const noexcept {
      size_t hash = std::hash<uint64_t>{}(key.node_id);
      hash ^= std::hash<uint64_t>{}(key.owner_id) + (hash << 6) + (hash >> 2);
      hash ^= std::hash<uint64_t>{}(key.port_id) + (hash << 6) + (hash >> 2);
      return hash;
    }
  };

  explicit PipeConsumeTracker(uint64_t ttl_ticks) : ttl_ticks_(ttl_ticks) {}

  // Records the pending consume for a shortfall the resolved source
  // (source_node_id != 0) must fill. Returns the emission decision; when it
  // is not request_source (no shortfall, no source, or zero request id) the
  // caller must answer the consumer immediately and nothing is recorded.
  // source_epoch binds the pending to the source port's epoch at plan time
  // (3.5.4); node-based fallback requests pass 0.
  ShortfallDecision planShortfall(uint64_t drain_request_id,
                                  uint64_t consume_request_id,
                                  uint64_t sink_node_id, uint32_t fluid_id,
                                  int32_t requested, int32_t pipe_accepted,
                                  uint64_t source_node_id,
                                  uint64_t source_owner_id,
                                  gtnh::common::PortId source_port_id,
                                  uint64_t now_tick,
                                  uint64_t source_epoch = 0) {
    ShortfallDecision decision;
    const int32_t shortfall = requested - pipe_accepted;
    if (shortfall <= 0 || source_node_id == 0 || drain_request_id == 0) {
      return decision;
    }
    // 3.5.3: a source inside its rejection cooldown is not probed again; the
    // caller short-fills the consume from the pipe part until the window
    // elapses. Suppressed plans record no pending and emit no request.
    if (sourceInRejectionBackoff(
            {source_node_id, source_owner_id, source_port_id}, now_tick)) {
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
    pending.source_epoch = source_epoch;
    pending.fluid_id = fluid_id;
    pending.requested = requested;
    pending.pipe_accepted = pipe_accepted;
    pending.expires_at = now_tick + ttl_ticks_;
    pending.retry_count = 0;
    pending.next_retry_at = now_tick + kDrainRetryBaseBackoffTicks;
    pending.cancelled = false;
    pending_.emplace(drain_request_id, pending);
    return decision;
  }

  // Applies a drain response exactly once (replay guard keyed by request id:
  // applied entries are erased, so a repeated response finds nothing).
  // Responses for unknown/expired ids, cancelled requests (3.5.3), or a
  // mismatched fluid id are ignored. The accepted amount is clamped to
  // [0, shortfall]: negative acceptance completes the consume as blocked
  // (zero source part), over-acceptance is capped so combined never exceeds
  // requested.
  DrainApplyResult applyDrainResponse(uint64_t drain_request_id,
                                      uint32_t fluid_id,
                                      int32_t accepted_amount) {
    DrainApplyResult result;
    auto it = pending_.find(drain_request_id);
    if (it == pending_.end()) return result;
    const PendingConsume pending = it->second;
    if (pending.cancelled) return result;  // 3.5.3: cancelled never applies
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

  // 3.5.3: pendings due for a retry at now_tick, with the next attempt
  // scheduled (doubling backoff, capped). Skips cancelled entries, entries at
  // the retry cap, and entries at or past the TTL deadline (expire() owns
  // that transition). At most one publish per pending is ever in flight.
  std::vector<DrainRetry> collectRetries(uint64_t now_tick) {
    std::vector<DrainRetry> retries;
    for (auto& entry : pending_) {
      PendingConsume& pending = entry.second;
      if (pending.cancelled) continue;
      if (pending.retry_count >= kMaxDrainRetries) continue;
      if (now_tick < pending.next_retry_at) continue;
      if (now_tick >= pending.expires_at) continue;
      ++pending.retry_count;
      pending.next_retry_at =
          now_tick + drainRetryBackoffTicks(pending.retry_count);
      retries.push_back(DrainRetry{
          pending.drain_request_id, pending.source_node_id,
          pending.source_owner_id, pending.source_port_id, pending.source_epoch,
          pending.fluid_id,
          pending.requested - pending.pipe_accepted, pending.retry_count});
    }
    return retries;
  }

  // 3.5.3: cancels one pending by request id (staleness detected late — e.g.
  // a response reveals the port is gone). The entry is MARKED cancelled and
  // kept until its TTL so a late response is recognized and dropped by the
  // exactly-once apply path. Returns the entry once, for the caller to
  // complete it as short-fill; unknown or already-cancelled ids return
  // nothing.
  std::vector<PendingConsume> cancelRequest(uint64_t drain_request_id) {
    std::vector<PendingConsume> cancelled;
    auto it = pending_.find(drain_request_id);
    if (it == pending_.end() || it->second.cancelled) return cancelled;
    it->second.cancelled = true;
    cancelled.push_back(it->second);
    return cancelled;
  }

  // 3.5.4(c): cancels every live pending addressed to (owner, port) whose
  // bound epoch differs from current_epoch — the port was re-registered or
  // redefined. Same-epoch pendings stay live (producers republish
  // idempotently per tick at the same epoch, 2.4.3). Each cancelled entry is
  // returned once, for the caller to complete it as short-fill.
  std::vector<PendingConsume> cancelStaleEpoch(uint64_t owner_id,
                                               gtnh::common::PortId port_id,
                                               uint64_t current_epoch) {
    std::vector<PendingConsume> cancelled;
    if (port_id == 0) return cancelled;
    for (auto& entry : pending_) {
      PendingConsume& pending = entry.second;
      if (pending.cancelled) continue;
      if (pending.source_owner_id != owner_id ||
          pending.source_port_id != port_id) {
        continue;
      }
      if (pending.source_epoch == current_epoch) continue;
      pending.cancelled = true;
      cancelled.push_back(pending);
    }
    return cancelled;
  }

  // Peeks a pending without mutating it (response-time port-liveness gating,
  // 3.5.4(a)/(c)).
  const PendingConsume* findPending(uint64_t drain_request_id) const {
    auto it = pending_.find(drain_request_id);
    return it == pending_.end() ? nullptr : &it->second;
  }

  // --- 3.5.3 rejection cooldown (see the constants above) -----------------
  // The caller supplies ticks explicitly (service_tick_); the tracker keeps
  // no clock of its own.

  bool sourceInRejectionBackoff(const SourceKey& key, uint64_t now_tick) const {
    auto it = reject_backoff_.find(key);
    return it != reject_backoff_.end() && now_tick < it->second.until_tick;
  }

  // Records a zero-accepted answer from `key` at `now_tick`, scheduling the
  // next fresh probe after the doubled window.
  void noteSourceRejection(const SourceKey& key, uint64_t now_tick) {
    SourceRejection& state = reject_backoff_[key];
    if (state.consecutive < 32) ++state.consecutive;
    state.until_tick = now_tick + sourceRejectBackoffTicks(state.consecutive);
  }

  // A productive answer proves the source alive: reset its cooldown.
  void clearSourceRejection(const SourceKey& key) { reject_backoff_.erase(key); }

  // The port (re-)registered or was removed: its incarnation changed, so the
  // recorded failures no longer describe the current source.
  void clearRejectionBackoffForPort(uint64_t owner_id,
                                    gtnh::common::PortId port_id) {
    for (auto it = reject_backoff_.begin(); it != reject_backoff_.end();) {
      if (it->first.owner_id == owner_id && it->first.port_id == port_id) {
        it = reject_backoff_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // 3.5.3: cancels every live pending tied to `node_id` as source or as sink
  // — the node was removed (pipe or machine block gone). Entries are MARKED
  // cancelled and returned once for the caller to complete them as
  // short-fill; they are kept until their TTL so a late response is
  // recognized and dropped by the exactly-once apply path, and expire()
  // erases them silently (never completed twice). The node's rejection
  // cooldown is dropped with it. Returns nothing for unknown nodes.
  std::vector<PendingConsume> cancelForNode(uint64_t node_id) {
    std::vector<PendingConsume> cancelled;
    if (node_id == 0) return cancelled;
    for (auto& entry : pending_) {
      PendingConsume& pending = entry.second;
      if (pending.cancelled) continue;
      if (pending.source_node_id != node_id &&
          pending.sink_node_id != node_id) {
        continue;
      }
      pending.cancelled = true;
      cancelled.push_back(pending);
    }
    for (auto it = reject_backoff_.begin(); it != reject_backoff_.end();) {
      if (it->first.node_id == node_id) {
        it = reject_backoff_.erase(it);
      } else {
        ++it;
      }
    }
    return cancelled;
  }

  // Erases pending consumes past their deadline and returns the live ones;
  // the caller completes each with its pipe-only amount (short-fill).
  // Cancelled entries were already completed at cancel time — they are erased
  // silently so they are never completed twice (3.5.3).
  std::vector<PendingConsume> expire(uint64_t now_tick) {
    std::vector<PendingConsume> expired;
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.expires_at <= now_tick) {
        if (!it->second.cancelled) expired.push_back(it->second);
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
    return expired;
  }

  // Completes every live pending addressed to the removed (owner, port) pair
  // (2.5.1/2.5.3). Node-based fallback pendings (port_id 0) are never
  // matched here — they are TTL-bound. Port ids are owner-scoped, so the
  // owner must match exactly. Already-cancelled entries are skipped: they
  // were completed when cancelled and must not complete twice.
  std::vector<PendingConsume> clearForPort(uint64_t owner_id,
                                           gtnh::common::PortId port_id) {
    std::vector<PendingConsume> cleared;
    if (port_id == 0) return cleared;
    clearRejectionBackoffForPort(owner_id, port_id);
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.source_port_id == port_id &&
          it->second.source_owner_id == owner_id &&
          !it->second.cancelled) {
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
  struct SourceRejection {
    uint64_t until_tick = 0;
    uint32_t consecutive = 0;
  };

  uint64_t ttl_ticks_ = 0;
  std::unordered_map<uint64_t, PendingConsume> pending_;
  std::unordered_map<SourceKey, SourceRejection, SourceKeyHash> reject_backoff_;
};

} // namespace pipe_network
} // namespace gtnh
