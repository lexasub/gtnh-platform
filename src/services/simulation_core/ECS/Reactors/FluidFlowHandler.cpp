#include "FluidFlowHandler.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/FluidStorage.h"
#include "core_generated.h"
#include "pipe_network_generated.h"
#include <spdlog/spdlog.h>

namespace simcore {

namespace {
// Stable slot for the FLUID buffer this handler reports; typed per-machine
// port allocation (openspec task 2.4.1) will replace it.
constexpr gtnh::common::PortId kFluidBufferPortId = 1;
} // namespace

FluidFlowHandler::FluidFlowHandler(entt::registry& reg,
                                   std::shared_ptr<ResourceBufferStatePublisher> statePublisher)
    : reg_(reg), statePublisher_(std::move(statePublisher))
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

    // Telemetry only (openspec 3.3.3/6.1): a fluid.flow event never debits or
    // credits any owner or pipe buffer, and must stay safe to observe twice.
    // Source debits go through typed ResourceDrainRequest transactions (3.2.x);
    // destination delivery goes through the typed fluid.consume.response path
    // (MachineSystem::onFluidConsumeResponse). Pipe buffers remain the only
    // authoritative transport-side amount.
    spdlog::trace("FluidFlowHandler: fluid {} x{} node {} -> {} at ({},{},{}) (telemetry only)",
                  fluid_id, amount, from_node, to_node, x, y, z);
    if (to_node == 0) return;

    // Client-facing snapshot (gateway route) of the destination owner's fluid
    // buffer. Read-only observation: publishes the component's current state
    // without mutating it. owner = the node identity PipeNetwork uses for
    // this machine; epoch 0 until typed ports supply generations.
    entt::entity entity = static_cast<entt::entity>(to_node);
    if (!reg_.valid(entity)) return;
    const auto* mc = reg_.try_get<MachineComponent>(entity);
    const auto* fluid = reg_.try_get<FluidStorage>(entity);
    if (!fluid) return;
    if (statePublisher_) {
        statePublisher_->PublishBufferState(
            to_node, kFluidBufferPortId, gtnh::common::ResourceKind::FLUID,
            fluid->fluid_id, fluid->amount, fluid->capacity, fluid->maxOutput,
            /*epoch=*/0, mc ? static_cast<int32_t>(mc->x) : x,
            mc ? static_cast<int32_t>(mc->y) : y,
            mc ? static_cast<int32_t>(mc->z) : z);
    }
}

} // namespace simcore
