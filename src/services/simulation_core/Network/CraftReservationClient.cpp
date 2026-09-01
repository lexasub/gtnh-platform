#include "Network/CraftReservationClient.h"

#include "ECS/components/EnergyStorage.h"
#include "ECS/components/RecipeProgress.h"
#include "recipe_manager_lib/RecipeTypes.h"

#include <spdlog/spdlog.h>

namespace simcore {

using gtnh::common::kTopicResourceConsumeRequest;
using gtnh::common::ResourceKind;

CraftReservationClient::CraftReservationClient(entt::registry& reg,
                                               PublishFn publish)
    : reg_(reg), publish_(std::move(publish)) {}

std::uint64_t CraftReservationClient::mintRequestId() {
    return next_request_id_++;
}

bool CraftReservationClient::publishConsumeRequest(
    std::uint64_t request_id, ResourceKind kind, std::uint32_t resource_id,
    std::int32_t amount) {
    if (!publish_) return false;

    // Two-stage Finish: the table-builder Finish() only returns the root
    // offset; the FlatBufferBuilder root must be finished explicitly before
    // GetBufferPointer (see ResourceDrainHandler.cpp).
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::ResourceConsumeRequestBuilder builder(fbb);
    builder.add_request_id(request_id);
    builder.add_port_id(0); // routing identity is the responder's concern
    builder.add_resource_kind(gtnh::common::ToWire(kind));
    builder.add_resource_id(resource_id);
    builder.add_amount(amount);
    fbb.Finish(builder.Finish());
    std::vector<std::uint8_t> payload(fbb.GetBufferPointer(),
                                      fbb.GetBufferPointer() + fbb.GetSize());
    if (!publish_(kTopicResourceConsumeRequest, payload)) {
        spdlog::warn("CraftReservationClient: failed to publish consume request {}",
                     request_id);
        return false;
    }
    return true;
}

bool CraftReservationClient::beginReservation(entt::entity entity,
                                              const RecipeManager::Recipe& recipe,
                                              std::uint64_t now_tick) {
    auto* progress = reg_.try_get<RecipeProgress>(entity);
    if (!progress || progress->pending_craft) return false;
    if (!recipe.hasResourceRequirements()) return false;

    PendingCraft craft;
    craft.recipe_id = recipe.id;
    craft.owner_id = static_cast<std::uint64_t>(entity);
    craft.entity_id = static_cast<std::uint64_t>(entity);
    craft.epoch = now_tick;
    craft.retry_count = 0;
    craft.expiry_tick = now_tick + kReservationExpiryTicks;
    craft.next_retry_tick = now_tick + kRetryBackoffTicks;

    bool requested_any = false;
    std::uint32_t requirement_index = 0;
    for (const auto& requirement : recipe.resource_requirements) {
        ++requirement_index;
        RequirementReservation reservation;
        reservation.requirement_id = requirement_index;
        reservation.kind = requirement.kind;
        reservation.resource_id = requirement.resource_id;
        reservation.required_amount = static_cast<std::int32_t>(requirement.amount);
        reservation.epoch = craft.epoch;

        const std::int32_t amount = reservation.remainingAmount();
        if (amount <= 0) {
            craft.requirements.push_back(reservation);
            continue;
        }
        const std::uint64_t request_id = mintRequestId();
        reservation.request_id = request_id;
        if (publishConsumeRequest(request_id, requirement.kind,
                                  requirement.resource_id, amount)) {
            outstanding_.emplace(request_id,
                                 OutstandingRequest{entity, requirement_index, amount});
            requested_any = true;
        } else {
            // Publish failure: leave request_id 0 so the next retry
            // re-requests this requirement instead of waiting for a
            // response that was never sent.
            reservation.request_id = 0;
        }
        craft.requirements.push_back(reservation);
        if (craft.request_id == 0) {
            craft.request_id = reservation.request_id;
        }
    }

    if (!requested_any) {
        // Nothing is in flight: keep the craft pending so the retry path
        // re-requests on the next backoff deadline (zero acceptance
        // semantics — inputs stay untouched).
        craft.next_retry_tick = now_tick;
        progress->pending_craft = std::move(craft);
        return false;
    }
    progress->pending_craft = std::move(craft);
    return true;
}

bool CraftReservationClient::tickPending(entt::entity entity,
                                         std::uint64_t now_tick) {
    auto* progress = reg_.try_get<RecipeProgress>(entity);
    if (!progress || !progress->pending_craft) return false;
    PendingCraft& craft = *progress->pending_craft;

    // A fully-accepted craft is commit-ready: no retry accounting and no
    // expiry may cancel it anymore.
    if (craft.fullyAccepted()) return true;

    if (craft.expired(now_tick) || craft.retry_count >= kMaxRetries) {
        cancel(entity, "reservation timeout");
        return false;
    }
    if (!craft.retryDue(now_tick)) return true;

    bool requested_any = false;
    for (auto& reservation : craft.requirements) {
        if (reservation.fullyAccepted()) continue;
        const std::int32_t amount = reservation.remainingAmount();
        if (amount <= 0) continue;

        // A fresh request id per retry: the previous response (if it ever
        // arrives) finds no outstanding entry and is ignored, so a retry can
        // never double-credit.
        const std::uint64_t request_id = mintRequestId();
        reservation.request_id = request_id;
        if (publishConsumeRequest(request_id, reservation.kind,
                                  reservation.resource_id, amount)) {
            outstanding_.emplace(request_id,
                                 OutstandingRequest{entity, reservation.requirement_id, amount});
            requested_any = true;
        } else {
            reservation.request_id = 0;
        }
    }
    ++craft.retry_count;
    craft.next_retry_tick = now_tick + kRetryBackoffTicks;
    if (!requested_any) {
        craft.next_retry_tick = now_tick;
    }
    return true;
}

entt::entity CraftReservationClient::onConsumeResponse(
    const gtnh::common::ResourceTransferResponse& response) {
    auto it = outstanding_.find(response.request_id);
    if (it == outstanding_.end()) {
        return entt::null; // unknown, duplicate, or late response
    }
    const OutstandingRequest pending = it->second;
    outstanding_.erase(it);

    const std::int32_t accepted =
        response.accepted_amount > 0
            ? (std::min)(response.accepted_amount, pending.requested)
            : 0;

    if (pending.requirement_id == 0) {
        auto count = charge_by_entity_.find(pending.entity);
        if (count != charge_by_entity_.end() && count->second > 1) {
            --count->second;
        } else {
            charge_by_entity_.erase(pending.entity);
        }
        creditBuffer(pending.entity, accepted);
        return entt::null;
    }

    if (!reg_.valid(pending.entity)) return entt::null;
    auto* progress = reg_.try_get<RecipeProgress>(pending.entity);
    if (!progress || !progress->pending_craft) return entt::null;
    auto* reservation =
        progress->pending_craft->findReservationByRequestId(response.request_id);
    if (!reservation || reservation->requirement_id != pending.requirement_id) {
        return entt::null;
    }
    creditBuffer(pending.entity, accepted);
    reservation->recordAccepted(accepted);
    if (progress->pending_craft->fullyAccepted()) {
        return pending.entity;
    }
    return entt::null;
}

void CraftReservationClient::cancel(entt::entity entity,
                                    std::string_view reason) {
    charge_by_entity_.erase(entity);
    for (auto it = outstanding_.begin(); it != outstanding_.end();) {
        if (it->second.entity == entity) {
            it = outstanding_.erase(it);
        } else {
            ++it;
        }
    }
    if (reg_.valid(entity)) {
        if (auto* progress = reg_.try_get<RecipeProgress>(entity)) {
            progress->clearPendingCraft();
        }
    }
    spdlog::debug("CraftReservationClient: cancelled pending craft for entity {} ({})",
                  static_cast<std::uint32_t>(entity), reason);
}

std::uint64_t CraftReservationClient::beginPerTickCharge(
    entt::entity entity, const RecipeManager::Recipe& recipe) {
    if (!recipe.hasResourceRequirements()) return 0;
    if (hasOutstandingCharge(entity)) return 0;

    // One charge request per requirement leg, so each keeps its own kind and
    // resource id (4.3.4: every recurring requirement goes through the same
    // accepted-amount path).
    std::uint64_t first_id = 0;
    for (const auto& requirement : recipe.resource_requirements) {
        const std::int32_t amount = static_cast<std::int32_t>(requirement.amount);
        if (amount <= 0) continue;
        const std::uint64_t request_id = mintRequestId();
        if (!publishConsumeRequest(request_id, requirement.kind,
                                   requirement.resource_id, amount)) {
            continue;
        }
        outstanding_.emplace(request_id, OutstandingRequest{entity, 0, amount});
        ++charge_by_entity_[entity];
        if (first_id == 0) first_id = request_id;
    }
    return first_id;
}

bool CraftReservationClient::hasOutstandingCharge(entt::entity entity) const {
    return charge_by_entity_.find(entity) != charge_by_entity_.end();
}

void CraftReservationClient::creditBuffer(entt::entity entity,
                                          std::int32_t amount) {
    if (amount <= 0 || !reg_.valid(entity)) return;
    if (auto* energy = reg_.try_get<EnergyStorage>(entity)) {
        energy->current = (std::min)(
            energy->current + amount, energy->capacity);
    }
}

} // namespace simcore
