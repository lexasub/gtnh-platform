#include "FluidFlowHandler.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/FluidStorage.h"
#include "core_generated.h"
#include "pipe_network_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

namespace {
// Stable slot for the FLUID buffer this handler credits; typed per-machine
// port allocation (openspec task 2.4.1) will replace it.
constexpr gtnh::common::PortId kFluidBufferPortId = 1;
} // namespace

FluidFlowHandler::FluidFlowHandler(entt::registry& reg,
                                   std::shared_ptr<FluidClient> fluidClient,
                                   std::shared_ptr<ResourceBufferStatePublisher> statePublisher)
    : reg_(reg), fluidClient_(std::move(fluidClient)),
      statePublisher_(std::move(statePublisher))
{}

void FluidFlowHandler::handle(const std::vector<uint8_t>& data) {
    auto* flow = flatbuffers::GetRoot<Protocol::FluidFlowEvent>(data.data());
    if (!flow || !flow->pos()) return;

    int32_t x = flow->pos()->x();
    int32_t y = flow->pos()->y();
    int32_t z = flow->pos()->z();
    uint64_t from_node = flow->from_node_id();
    uint64_t to_node = flow->to_node_id();
    int32_t amount = flow->amount();
    uint32_t fluid_id = flow->fluid_id();
    if (from_node == 0 || amount <= 0) return;

    // Source-drain events are telemetry only. PipeNetworkService has already
    // debited the authoritative source snapshot; repeating that debit here
    // would double-debit the ECS owner and would be unsafe on replay.
    if (to_node == 0) {
        spdlog::trace("FluidFlowHandler: observed source fluid debit node={} fluid={} x{} at ({},{},{})",
                      from_node, fluid_id, amount, x, y, z);
        return;
    }

    // A destination flow (when emitted by a future network transport path) may
    // credit its owner. Route by the explicit destination id, never by event
    // position, so source positions cannot credit the wrong machine.
    entt::entity entity = static_cast<entt::entity>(to_node);
    if (!reg_.valid(entity)) {
        spdlog::trace("FluidFlowHandler: destination entity {} is not valid", to_node);
        return;
    }
    auto* mc = reg_.try_get<MachineComponent>(entity);
    auto* fluid = reg_.try_get<FluidStorage>(entity);
    if (!fluid) {
        spdlog::debug("FluidFlowHandler: destination entity {} has no FluidStorage", to_node);
        return;
    }

    bool faceAllowsFluid = true;
    if (mc) {
        faceAllowsFluid = false;
        for (int f = 0; f < 6; ++f) {
            uint8_t role = mc->getFaceRole(f);
            if (role == static_cast<uint8_t>(MachineFaceRole::FLUID_IN) ||
                role == static_cast<uint8_t>(MachineFaceRole::NONE)) {
                faceAllowsFluid = true;
                break;
            }
        }
    }
    if (!faceAllowsFluid) {
        spdlog::debug("FluidFlowHandler: destination node {} has no FLUID_IN face", to_node);
        return;
    }

    const int32_t accepted = fluid->addFluid(amount);
    if (fluidClient_) {
        fluidClient_->publishNodeUpdate(
            to_node, mc ? mc->x : x, mc ? mc->y : y, mc ? mc->z : z,
            fluid->fluid_id, fluid->amount, fluid->capacity,
            0, 0, 0, false, true);
    }
    // Client-facing snapshot (gateway route), separate from the internal
    // PipeNetwork node update above. owner = the node identity PipeNetwork
    // uses for this machine; epoch 0 until typed ports supply generations.
    if (statePublisher_) {
        statePublisher_->PublishBufferState(
            to_node, kFluidBufferPortId, gtnh::common::ResourceKind::FLUID,
            fluid->fluid_id, fluid->amount, fluid->capacity, fluid->maxOutput,
            /*epoch=*/0, mc ? static_cast<int32_t>(mc->x) : x,
            mc ? static_cast<int32_t>(mc->y) : y,
            mc ? static_cast<int32_t>(mc->z) : z);
    }
    spdlog::trace("FluidFlowHandler: fluid {} x{} delivered to destination node {} accepted={}",
                  fluid_id, amount, to_node, accepted);
}

} // namespace simcore
