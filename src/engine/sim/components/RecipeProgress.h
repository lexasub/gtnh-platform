#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <numeric>
#include <string>
#include <vector>

#if __has_include("common/ResourcePort.h")
#include "common/ResourcePort.h"
#define SIMCORE_RECIPE_PROGRESS_HAS_COMMON_RESOURCE_KIND 1
#endif

namespace simcore {

#if SIMCORE_RECIPE_PROGRESS_HAS_COMMON_RESOURCE_KIND
using ResourceKind = gtnh::common::ResourceKind;
#else
// Temporary compatibility type. The shared gtnh::common::ResourceKind is used
// automatically when the common resource-port contract is available.
namespace recipe_progress_detail {
enum class TemporaryResourceKind : std::uint8_t {
  FLUID = 0,
  EU = 1,
  HU = 2,
  RU = 3,
  ITEM = 4,
};
} // namespace recipe_progress_detail
using ResourceKind = recipe_progress_detail::TemporaryResourceKind;
#endif

// One externally supplied recipe requirement and its reservation. A request
// is tracked on the reservation itself so responses can be matched by request
// ID and requirement, rather than by arrival order.
struct RequirementReservation {
  std::uint32_t requirement_id = 0;
  ResourceKind kind = ResourceKind::FLUID;
  std::uint32_t resource_id = 0;
  std::int32_t required_amount = 0;
  std::int32_t accepted_amount = 0;
  std::uint64_t request_id = 0;
  std::uint64_t epoch = 0;

  [[nodiscard]] std::int32_t remainingAmount() const noexcept {
    return (std::max)(required_amount - accepted_amount, 0);
  }

  [[nodiscard]] bool fullyAccepted() const noexcept {
    return required_amount > 0 && accepted_amount >= required_amount;
  }

  [[nodiscard]] bool partial() const noexcept {
    return accepted_amount > 0 && !fullyAccepted();
  }

  // Responses are cumulative. Clamping here prevents a duplicate or malformed
  // response from making the reservation appear to have accepted too much.
  void recordAccepted(std::int32_t amount) noexcept {
    if (amount <= 0 || required_amount <= 0) {
      return;
    }
    accepted_amount +=
        (std::min)(amount, remainingAmount());
  }

  void clear() noexcept {
    requirement_id = 0;
    kind = ResourceKind::FLUID;
    resource_id = 0;
    required_amount = 0;
    accepted_amount = 0;
    request_id = 0;
    epoch = 0;
  }
};

using ResourceReservation = RequirementReservation;

// State held while SimulationCore is obtaining all non-item resources for a
// craft. Input inventory must remain untouched until fullyAccepted() is true.
struct PendingCraft {
  std::string recipe_id;
  std::uint64_t owner_id = 0;
  std::uint64_t entity_id = 0;
  std::vector<RequirementReservation> requirements;

  // Transaction-level correlation ID. Individual requirements retain their
  // own request IDs above; this field supports a single-request reservation
  // and gives persistence a stable craft-level identity.
  std::uint64_t request_id = 0;
  std::uint64_t epoch = 0;
  std::uint32_t retry_count = 0;
  std::uint64_t expiry_tick = 0;
  std::uint64_t next_retry_tick = 0;

  [[nodiscard]] std::vector<RequirementReservation>&
  reservations() noexcept {
    return requirements;
  }

  [[nodiscard]] const std::vector<RequirementReservation>&
  reservations() const noexcept {
    return requirements;
  }

  // Response correlation is by request ID and requirement (4.3.1), never by
  // arrival order: the reservation carries the request id it was issued with.
  [[nodiscard]] RequirementReservation*
  findReservationByRequestId(std::uint64_t response_request_id) noexcept {
    for (auto& requirement : requirements) {
      if (requirement.request_id != 0 &&
          requirement.request_id == response_request_id) {
        return &requirement;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool fullyAccepted() const noexcept {
    if (requirements.empty()) {
      return false;
    }
    return std::all_of(requirements.begin(), requirements.end(),
                       [](const RequirementReservation& requirement) {
                         return requirement.fullyAccepted();
                       });
  }

  [[nodiscard]] bool isFullyAccepted() const noexcept {
    return fullyAccepted();
  }

  [[nodiscard]] std::int32_t requiredAmount() const noexcept {
    return std::accumulate(
        requirements.begin(), requirements.end(), std::int32_t{0},
        [](std::int32_t total, const RequirementReservation& requirement) {
          return total + (std::max)(requirement.required_amount, 0);
        });
  }

  [[nodiscard]] std::int32_t acceptedAmount() const noexcept {
    return std::accumulate(
        requirements.begin(), requirements.end(), std::int32_t{0},
        [](std::int32_t total, const RequirementReservation& requirement) {
          return total + (std::clamp)(requirement.accepted_amount, 0,
                                      (std::max)(requirement.required_amount, 0));
        });
  }

  [[nodiscard]] std::int32_t remainingAmount() const noexcept {
    return (std::max)(requiredAmount() - acceptedAmount(), 0);
  }

  [[nodiscard]] bool partial() const noexcept {
    if (fullyAccepted()) {
      return false;
    }
    return std::any_of(requirements.begin(), requirements.end(),
                       [](const RequirementReservation& requirement) {
                         return requirement.partial();
                       });
  }

  [[nodiscard]] bool isPartial() const noexcept { return partial(); }

  [[nodiscard]] bool expired(std::uint64_t now_tick) const noexcept {
    return expiry_tick != 0 && now_tick >= expiry_tick;
  }

  [[nodiscard]] bool retryDue(std::uint64_t now_tick) const noexcept {
    return now_tick >= next_retry_tick;
  }

  // Drop all identity, reservation, and retry state when a craft is cancelled
  // or its recipe/owner/epoch is no longer valid.
  void clear() noexcept {
    recipe_id.clear();
    owner_id = 0;
    entity_id = 0;
    requirements.clear();
    request_id = 0;
    epoch = 0;
    retry_count = 0;
    expiry_tick = 0;
    next_retry_tick = 0;
  }
};

struct RecipeProgress {
  std::string recipe_id;        // current recipe ID, empty string if idle
  std::uint32_t remaining_ticks = 0; // ticks left until completion
  bool is_processing = false;   // true while recipe is active
  bool needs_output = false;    // true when recipe complete, output pending

  // Set only while waiting for external resource reservations. Input items
  // must not be consumed until pending_craft->fullyAccepted() is true.
  std::optional<PendingCraft> pending_craft;

  void clearPendingCraft() noexcept { pending_craft.reset(); }

  // Default constructor: all zeros, empty recipe_id, no pending craft
  RecipeProgress() = default;
};

} // namespace simcore

#ifdef SIMCORE_RECIPE_PROGRESS_HAS_COMMON_RESOURCE_KIND
#undef SIMCORE_RECIPE_PROGRESS_HAS_COMMON_RESOURCE_KIND
#endif
