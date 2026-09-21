#include "Network/ResourceDrainHandler.h"

#include <engine/sim/components/FluidStorage.h>
#include <engine/sim/components/SteamOutputComponent.h>

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace simcore {

using gtnh::common::kTopicResourceDrainRequest;
using gtnh::common::kTopicResourceDrainResponse;
using gtnh::common::kTopicResourcePortRegister;
using gtnh::common::kTopicResourcePortRemove;
using gtnh::common::ParseDrainRequest;
using gtnh::common::ParsePortRegister;
using gtnh::common::ParsePortRemove;
using gtnh::common::ResourceTransferRequest;
using gtnh::common::ResourceTransferResponse;

namespace {

// ResourcePortClient.h provides Parse helpers for responses but no response
// serializer; build the wire payload here with the generated table builder.
// The generated table-builder Finish() only closes the table and returns the
// root offset: the FlatBufferBuilder root itself must be finished explicitly
// (fbb.Finish(builder.Finish())) before GetBufferPointer.
std::vector<std::uint8_t> serializeDrainResponse(
    const gtnh::common::ResourceTransferResponse& response) {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::ResourceDrainResponseBuilder builder(fbb);
    builder.add_request_id(response.request_id);
    builder.add_port_id(response.port_id);
    builder.add_resource_kind(gtnh::common::ToWire(response.resource_kind));
    builder.add_resource_id(response.resource_id);
    builder.add_accepted_amount(response.accepted_amount);
    fbb.Finish(builder.Finish());
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

// Local ResourcePortRemove serializer: gtnh::common::SerializePortRemove
// discards the table-builder offset and reads an unfinished buffer, which
// asserts; src/common is outside this change's edit scope.
std::vector<std::uint8_t> serializePortRemove(std::uint64_t owner_id,
                                              gtnh::common::ResourceKind kind,
                                              gtnh::common::PortId port_id,
                                              std::uint64_t epoch) {
    flatbuffers::FlatBufferBuilder fbb;
    Protocol::ResourcePortRemoveBuilder builder(fbb);
    builder.add_port_id(port_id);
    builder.add_owner_id(owner_id);
    builder.add_epoch(epoch);
    builder.add_resource_kind(gtnh::common::ToWire(kind));
    fbb.Finish(builder.Finish());
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

} // namespace

ResourceDrainHandler::ResourceDrainHandler(entt::registry& reg, PublishFn publish,
                                           std::uint16_t steam_item_id)
    : reg_(reg), publish_(std::move(publish)), steam_id_(steam_item_id) {}

void ResourceDrainHandler::handlePortRegister(const std::vector<std::uint8_t>& data) {
    gtnh::common::ResourcePort port;
    std::uint32_t resource_id = 0;
    if (!ParsePortRegister(data.data(), data.size(), &port, &resource_id)) {
        spdlog::warn("ResourceDrainHandler: malformed ResourcePortRegister ({} bytes)", data.size());
        return;
    }
    if (port.port_id == 0) {
        spdlog::warn("ResourceDrainHandler: ignoring port register with port_id=0 (owner={})",
                     port.owner_id);
        return;
    }

    auto it = ports_.find(port.port_id);
    if (it != ports_.end()) {
        const auto& existing = it->second;
        if (existing.owner_id == port.owner_id &&
            existing.resource_kind == port.resource_kind &&
            existing.epoch == port.epoch) {
            // Idempotent re-registration by (owner, kind, port, epoch).
            it->second = port;
            return;
        }
        if (port.epoch < existing.epoch) {
            spdlog::debug("ResourceDrainHandler: stale epoch {} for port {} (current {}), ignored",
                          port.epoch, port.port_id, existing.epoch);
            return;
        }
        if (existing.owner_id != port.owner_id ||
            existing.resource_kind != port.resource_kind) {
            spdlog::warn("ResourceDrainHandler: port {} re-registered by different (owner, kind): "
                         "old=({},{}) new=({},{})",
                         port.port_id, existing.owner_id,
                         static_cast<int>(existing.resource_kind), port.owner_id,
                         static_cast<int>(port.resource_kind));
        }
    }
    ports_[port.port_id] = port;
    spdlog::debug("ResourceDrainHandler: registered port {} owner={} kind={} role={} epoch={} rate={} cap={}",
                  port.port_id, port.owner_id, static_cast<int>(port.resource_kind),
                  static_cast<int>(port.role), port.epoch, port.rate, port.capacity);
}

void ResourceDrainHandler::handlePortRemove(const std::vector<std::uint8_t>& data) {
    gtnh::common::ResourcePortRegistrationKey key;
    if (!ParsePortRemove(data.data(), data.size(), &key)) {
        spdlog::warn("ResourceDrainHandler: malformed ResourcePortRemove ({} bytes)", data.size());
        return;
    }

    auto it = ports_.find(key.port_id);
    if (it == ports_.end()) return;
    if (it->second.owner_id != key.owner_id ||
        it->second.resource_kind != key.resource_kind ||
        it->second.epoch != key.epoch) {
        return; // not an exact match — keep the port
    }
    ports_.erase(it);
    for (auto rit = replay_.begin(); rit != replay_.end();) {
        if (rit->second.response.port_id == key.port_id) {
            rit = replay_.erase(rit);
        } else {
            ++rit;
        }
    }
    spdlog::debug("ResourceDrainHandler: removed port {} owner={} epoch={}",
                  key.port_id, key.owner_id, key.epoch);
}

void ResourceDrainHandler::handleDrainRequest(const std::vector<std::uint8_t>& data) {
    ResourceTransferRequest request;
    if (!ParseDrainRequest(data.data(), data.size(), &request)) {
        spdlog::warn("ResourceDrainHandler: malformed ResourceDrainRequest ({} bytes)", data.size());
        return;
    }

    // request_id 0 is not a transaction identity: it cannot be replay-cached,
    // so answering it with a debit could double-debit on redelivery.
    if (request.request_id == 0) {
        spdlog::warn("ResourceDrainHandler: drain request with request_id=0 rejected");
        ResourceTransferResponse zero;
        zero.request_id = 0;
        zero.port_id = request.port_id;
        zero.resource_kind = request.resource_kind;
        zero.resource_id = request.resource_id;
        zero.accepted_amount = 0;
        publishResponse(zero);
        return;
    }

    // Replay: a repeated request_id returns the cached response without a
    // second debit, even if port or buffer state changed in between.
    auto rit = replay_.find(request.request_id);
    if (rit != replay_.end()) {
        spdlog::debug("ResourceDrainHandler: replay of request {} -> accepted={}",
                      request.request_id, rit->second.response.accepted_amount);
        publishResponse(rit->second.response);
        return;
    }

    ResourceTransferResponse response;
    response.request_id = request.request_id;
    response.port_id = request.port_id;
    response.resource_kind = request.resource_kind;
    response.resource_id = request.resource_id;
    response.accepted_amount = 0;

    auto pit = ports_.find(request.port_id);
    if (pit == ports_.end()) {
        // Unknown or already-removed port: zero acceptance, cached so retries
        // stay deterministic.
        spdlog::debug("ResourceDrainHandler: drain for unknown port {} rejected", request.port_id);
        cacheResponse(request.request_id, {response, 0});
        publishResponse(response);
        return;
    }

    const gtnh::common::ResourcePort& port = pit->second;
    ReplayEntry entry{response, port.owner_id};

    if (port.role != gtnh::common::PortRole::SOURCE) {
        spdlog::debug("ResourceDrainHandler: port {} is not a SOURCE (role={})",
                      port.port_id, static_cast<int>(port.role));
    } else if (port.resource_kind != request.resource_kind) {
        spdlog::debug("ResourceDrainHandler: port {} kind {} != request kind {}",
                      port.port_id, static_cast<int>(port.resource_kind),
                      static_cast<int>(request.resource_kind));
    } else if (request.amount <= 0) {
        spdlog::debug("ResourceDrainHandler: non-positive amount {} on port {}",
                      request.amount, port.port_id);
    } else if (request.resource_kind != gtnh::common::ResourceKind::FLUID) {
        // Owner-side draining is implemented for FLUID only; EU/HU/RU/ITEM
        // still use their legacy paths and must not be debited here.
        spdlog::debug("ResourceDrainHandler: unsupported kind {} on port {}",
                      static_cast<int>(request.resource_kind), port.port_id);
    } else {
        entry.response.accepted_amount = drainMachineBuffer(port, request);
    }

    cacheResponse(request.request_id, entry);
    publishResponse(entry.response);
}

std::int32_t ResourceDrainHandler::drainMachineBuffer(
    const gtnh::common::ResourcePort& port, const ResourceTransferRequest& request) {
    // The owner id is the EnTT entity value the registration path used
    // (node/owner identity convention: static_cast<uint64_t>(entity)). A
    // destroyed or replaced machine yields an invalid or different entity, so
    // the stale port drains nothing — never fall back to a position lookup,
    // which would drain the replacement machine.
    auto entity = static_cast<entt::entity>(port.owner_id);
    if (!reg_.valid(entity)) {
        spdlog::debug("ResourceDrainHandler: owner {} of port {} is gone",
                      port.owner_id, port.port_id);
        return 0;
    }
    const std::int32_t rate_limit = port.rate > 0 ? port.rate : request.amount;
    if (reg_.all_of<SteamOutputComponent>(entity)) {
        return drainSteamOutput(entity, request, rate_limit);
    }
    if (reg_.all_of<FluidStorage>(entity)) {
        return drainFluidStorage(entity, request, rate_limit);
    }
    spdlog::debug("ResourceDrainHandler: owner {} of port {} has no fluid buffer",
                  port.owner_id, port.port_id);
    return 0;
}

std::int32_t ResourceDrainHandler::drainSteamOutput(
    entt::entity entity, const ResourceTransferRequest& request, std::int32_t rate_limit) {
    auto& steam = reg_.get<SteamOutputComponent>(entity);
    // SteamOutputComponent stores steam implicitly: the only fluid it can
    // drain is the carried registry-resolved Steam id. Fail closed on 0 so a
    // zero resource_id request can never match.
    if (steam_id_ == 0 || request.resource_id != steam_id_) {
        spdlog::debug("ResourceDrainHandler: steam buffer fluid mismatch: request {} != steam {}",
                      request.resource_id, steam_id_);
        return 0;
    }
    const double available = std::floor(std::max(0.0, steam.steam_stored));
    const double wanted = static_cast<double>(std::min(request.amount, rate_limit));
    const auto accepted = static_cast<std::int32_t>(std::min(wanted, available));
    if (accepted <= 0) return 0;
    steam.steam_stored -= static_cast<double>(accepted);
    return accepted;
}

std::int32_t ResourceDrainHandler::drainFluidStorage(
    entt::entity entity, const ResourceTransferRequest& request, std::int32_t rate_limit) {
    auto& fluid = reg_.get<FluidStorage>(entity);
    if (fluid.fluid_id == 0 || request.resource_id != fluid.fluid_id) {
        spdlog::debug("ResourceDrainHandler: FluidStorage fluid mismatch: request {} != buffer {}",
                      request.resource_id, fluid.fluid_id);
        return 0;
    }
    // removeFluid caps by availability and the machine's own maxOutput; the
    // returned value is the exact debited amount and becomes the response.
    const std::int32_t candidate = std::min(request.amount, rate_limit);
    return fluid.removeFluid(candidate);
}

void ResourceDrainHandler::removeOwnerPorts(std::uint64_t owner_id) {
    bool removed_any = false;
    for (auto it = ports_.begin(); it != ports_.end();) {
        if (it->second.owner_id != owner_id) {
            ++it;
            continue;
        }
        const gtnh::common::ResourcePort& port = it->second;
        if (publish_) {
            const std::vector<std::uint8_t> payload =
                serializePortRemove(owner_id, port.resource_kind, port.port_id, port.epoch);
            if (!publish_(kTopicResourcePortRemove, payload)) {
                spdlog::warn("ResourceDrainHandler: failed to publish port remove for port {}",
                             port.port_id);
            }
        }
        spdlog::debug("ResourceDrainHandler: owner {} removed port {} epoch {}",
                      owner_id, port.port_id, port.epoch);
        it = ports_.erase(it);
        removed_any = true;
    }
    if (!removed_any) return;

    for (auto rit = replay_.begin(); rit != replay_.end();) {
        if (rit->second.owner_id == owner_id) {
            rit = replay_.erase(rit);
        } else {
            ++rit;
        }
    }
    // Drop dangling order entries lazily during eviction (cacheResponse).
}

void ResourceDrainHandler::cacheResponse(std::uint64_t request_id, const ReplayEntry& entry) {
    auto [it, inserted] = replay_.try_emplace(request_id, entry);
    if (!inserted) return;
    replay_order_.push_back(request_id);
    while (replay_.size() > kReplayCacheMaxEntries) {
        while (!replay_order_.empty() && replay_.find(replay_order_.front()) == replay_.end()) {
            replay_order_.pop_front();
        }
        if (replay_order_.empty()) {
            replay_.clear();
            return;
        }
        replay_.erase(replay_order_.front());
        replay_order_.pop_front();
    }
}

void ResourceDrainHandler::publishResponse(const ResourceTransferResponse& response) {
    if (!publish_) return;
    const std::vector<std::uint8_t> payload = serializeDrainResponse(response);
    if (!publish_(kTopicResourceDrainResponse, payload)) {
        spdlog::warn("ResourceDrainHandler: failed to publish drain response for request {}",
                     response.request_id);
    }
}

} // namespace simcore
