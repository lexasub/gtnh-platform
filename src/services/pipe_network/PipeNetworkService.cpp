#include "PipeNetworkService.h"
#include "Client/MessageRouterClient.h"
#include <pipe_network_generated.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace {

// Bridge struct to pass private PipeNetworkService state into generic handlers
struct CheckBridge {
    std::unordered_map<uint64_t, uint64_t>& protocol_to_mgr;
    pipenet::PipeNetworkManager& network_manager;
    std::unordered_map<uint64_t, gtnh::pipe_network::NodeState>& node_states;
    gtnh::pipe_network::MessageRouterClient& router;
};

// Generic check handler: iterates network, sums source energy, publishes response.
template<typename ReqT, typename RespT>
void handleCheckTemplate(
    const std::vector<uint8_t>& data,
    const std::string& responseTopic,
    const CheckBridge& br,
    flatbuffers::Offset<RespT> (*createResp)(::flatbuffers::FlatBufferBuilder&, int32_t, int32_t))
{
    auto* req = flatbuffers::GetRoot<ReqT>(data.data());
    if (!req || !req->pos()) return;

    auto pit = br.protocol_to_mgr.find(req->node_id());
    if (pit == br.protocol_to_mgr.end()) {
        flatbuffers::FlatBufferBuilder fbb;
        auto resp = createResp(fbb, 0, req->demand());
        fbb.Finish(resp);
        br.router.Publish(responseTopic,
            {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
        return;
    }

    uint64_t mgr_id = pit->second;
    int32_t available = 0;
    for (const auto* net : br.network_manager.getAllNetworks()) {
        if (!net) continue;
        bool found = false;
        for (uint64_t nid : net->nodeIds)
            if (nid == mgr_id) { found = true; break; }
        if (!found) continue;

        for (uint64_t nid : net->nodeIds) {
            auto si = br.node_states.find(nid);
            if (si == br.node_states.end()) continue;
            if (si->second.is_source)
                available += si->second.energy;
        }
        break;
    }

    int32_t deficit = (std::max)(0, req->demand() - available);
    available = (std::min)(available, req->demand());

    flatbuffers::FlatBufferBuilder fbb;
    auto resp = createResp(fbb, available, deficit);
    fbb.Finish(resp);
    br.router.Publish(responseTopic,
        {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
}

// Shared consume computation: find network, fill sink, drain sources proportionally.
int32_t computeConsume(uint64_t mgr_id, int32_t amount,
                       CheckBridge& br, int32_t& out_total_source, int& out_source_count)
{
    int32_t consumed = 0;
    for (const auto* net : br.network_manager.getAllNetworks()) {
        if (!net) continue;
        bool found = false;
        for (uint64_t nid : net->nodeIds)
            if (nid == mgr_id) { found = true; break; }
        if (!found) continue;

        auto sink_it = br.node_states.find(mgr_id);
        if (sink_it != br.node_states.end()) {
            int32_t room = sink_it->second.capacity - sink_it->second.energy;
            int32_t give = (std::min)(amount, room);
            sink_it->second.energy += give;
            consumed += give;
            amount -= give;
        }

        for (uint64_t nid : net->nodeIds) {
            auto si = br.node_states.find(nid);
            if (si == br.node_states.end()) continue;
            if (si->second.is_source) {
                out_total_source += si->second.energy;
                ++out_source_count;
            }
        }
        break;
    }
    return consumed;
}

} // anonymous namespace

namespace gtnh {
namespace pipe_network {

PipeNetworkService::PipeNetworkService(MessageRouterClient& router, asio::io_context& io)
    : router_(router), io_(io), tick_timer_(io)
{}

PipeNetworkService::~PipeNetworkService() { Stop(); }

void PipeNetworkService::Start() {
    spdlog::info("PipeNetworkService starting");

    router_.OnMessage([this](const std::string& topic, const std::vector<uint8_t>& data) {
        onRouterMessage(topic, data);
    });

    router_.Subscribe("energy.node.update");
    router_.Subscribe("energy.check.request");
    router_.Subscribe("energy.consume.request");
    router_.Subscribe("fluid.node.update");
    router_.Subscribe("fluid.check.request");
    router_.Subscribe("fluid.consume.request");
    router_.Subscribe("item.node.update");
    router_.Subscribe("item.transfer.request");
    router_.Subscribe("world.blocks.changed");

    running_ = true;
    scheduleTick();
    spdlog::info("PipeNetworkService ready");
}

void PipeNetworkService::Stop() {
    running_ = false;
    asio::error_code ec;
    tick_timer_.cancel(ec);
}

void PipeNetworkService::scheduleTick() {
    if (!running_) return;
    tick_timer_.expires_after(std::chrono::milliseconds(TICK_INTERVAL_MS));
    tick_timer_.async_wait([this](std::error_code ec) {
        if (ec) return;
        tick();
        scheduleTick();
    });
}

void PipeNetworkService::tick() {
    for (const auto* net : network_manager_.getAllNetworks()) {
        if (!net || net->nodeIds.empty()) continue;

        int sourceCount = 0, sinkCount = 0;
        for (uint64_t nid : net->nodeIds) {
            auto si = node_states_.find(nid);
            if (si == node_states_.end()) continue;
            if (si->second.is_source) ++sourceCount;
            if (si->second.is_sink) ++sinkCount;
        }
        if (sourceCount > 0 || sinkCount > 0) {
            spdlog::trace("PipeNetwork #{}: {} nodes, {} sources, {} sinks",
                          net->id, net->nodeIds.size(), sourceCount, sinkCount);
        }
    }
    
    // Tick item networks — move items through pipes
    network_manager_.tickItemNetworks();

    // Publish item flow events for items consumed at machine sinks
    for (const auto& ev : network_manager_.getConsumedItemEvents()) {
        Protocol::Vec3i pos(ev.x, ev.y, ev.z);
        flatbuffers::FlatBufferBuilder fbb;
        auto event = Protocol::CreateItemFlowEvent(
            fbb, ev.sinkNodeId, ev.sourceNodeId, 0,
            ev.item.item_id, ev.item.count, &pos, 0);
        fbb.Finish(event);
        router_.Publish("item.flow",
            {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
        spdlog::debug("[PipeNet] item {} x{} consumed at machine node {} ({},{},{})",
                       ev.item.item_id, ev.item.count, ev.sinkNodeId, ev.x, ev.y, ev.z);
    }

    // Tick the cable graph for packet-based electricity transport
    cable_graph_.tick();

    for (const auto& exploded : cable_graph_.getExplodedNodes()) {
        Protocol::Vec3i pos(exploded.x, exploded.y, exploded.z);
        flatbuffers::FlatBufferBuilder fbb;
        auto event = Protocol::CreateCableExplodedEvent(fbb, exploded.nodeId, &pos, exploded.temperature);
        fbb.Finish(event);
        router_.Publish("energy.cable.exploded",
            {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
        spdlog::warn("[PipeNet] cable node {} at ({},{},{}) exploded - event published",
                     exploded.nodeId, exploded.x, exploded.y, exploded.z);
    }
}

void PipeNetworkService::onRouterMessage(const std::string& topic, const std::vector<uint8_t>& data) {
    if (topic == "energy.node.update") {
        handleNodeUpdate(data);
    } else if (topic == "energy.check.request") {
        handleCheckRequest(data);
    } else if (topic == "energy.consume.request") {
        handleConsumeRequest(data);
    } else if (topic == "fluid.node.update") {
        handleFluidNodeUpdate(data);
    } else if (topic == "fluid.check.request") {
        handleFluidCheckRequest(data);
    } else if (topic == "fluid.consume.request") {
        handleFluidConsumeRequest(data);
    } else if (topic == "item.node.update") {
        handleItemNodeUpdate(data);
    } else if (topic == "item.transfer.request") {
        handleItemTransferRequest(data);
    } else if (topic == "world.blocks.changed") {
        handleBlockChanged(data);
    }
}

void PipeNetworkService::handleBlockChanged(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!verifier.VerifyBuffer<Protocol::BlockChangedEvent>()) {
        spdlog::warn("[PipeNet] invalid BlockChangedEvent");
        return;
    }

    const auto* event = flatbuffers::GetRoot<Protocol::BlockChangedEvent>(data.data());
    auto* pos = event->pos();
    if (!pos) {
        spdlog::warn("[PipeNet] BlockChangedEvent missing pos");
        return;
    }

    int32_t x = pos->x();
    int32_t y = pos->y();
    int32_t z = pos->z();
    uint16_t block_id = event->block_id();
    uint64_t key = posKey(x, y, z);

    if (block_id == 0) {
        auto it = pipe_nodes_.find(key);
        if (it != pipe_nodes_.end()) {
            network_manager_.removeNode(it->second);
            pipe_nodes_.erase(it);
            spdlog::debug("[PipeNet] pipe node at ({},{},{}) removed", x, y, z);
        }
        return;
    }

    if (!isPipeBlock(block_id)) return;

    if (pipe_nodes_.find(key) == pipe_nodes_.end()) {
        uint64_t nodeId = network_manager_.addNode(x, y, z, block_id);
        pipe_nodes_.emplace(key, nodeId);
        spdlog::debug("[PipeNet] pipe node {} at ({},{},{}) added", nodeId, x, y, z);
    }

    // Trigger item network rebuilds for pipe block changes
    network_manager_.rebuildItemNetworks();
    
    // Handle cable blocks separately for packet-based electricity transport
    if (isCableBlock(block_id)) {
        const auto* cableDef = getCableDef(block_id);
        if (cableDef) {
            // Add cable node to CableGraph for packet-based electricity
            cable_graph_.addCableNode(key, *cableDef, x, y, z);
            spdlog::debug("[PipeNet] cable node added at ({},{},{})", x, y, z);
        }
    }
}

bool PipeNetworkService::isPipeBlock(uint16_t block_id) {
    switch (block_id) {
        case BLOCK_ID_ITEM_PIPE:
        case BLOCK_ID_FLUID_PIPE:
        case BLOCK_ID_DENSE_ITEM_PIPE:
        case BLOCK_ID_DENSE_FLUID_PIPE:
            return true;
        default:
            return false;
    }
}

bool PipeNetworkService::isCableBlock(uint16_t block_id) {
    return CABLE_DEFS.count(block_id) > 0;
}

uint64_t PipeNetworkService::posKey(int32_t x, int32_t y, int32_t z) {
    return (static_cast<uint64_t>(static_cast<int64_t>(x)) << 42)
         | (static_cast<uint64_t>(static_cast<int64_t>(y) & 0xFFFFF) << 20)
         | (static_cast<uint64_t>(static_cast<int64_t>(z) & 0xFFFFF));
}

void PipeNetworkService::handleNodeUpdate(const std::vector<uint8_t>& data) {
    auto* update = flatbuffers::GetRoot<Protocol::EnergyNodeUpdate>(data.data());
    if (!update || !update->pos()) return;

    uint64_t protocol_id = update->node_id();
    int32_t x = update->pos()->x();
    int32_t y = update->pos()->y();
    int32_t z = update->pos()->z();

    auto it = protocol_to_mgr_.find(protocol_id);
    uint64_t mgr_id;
    if (it == protocol_to_mgr_.end()) {
        if (!network_manager_.addNodeWithId(protocol_id, x, y, z, 1)) {
            spdlog::warn("Duplicate energy node {}", protocol_id);
            return;
        }
        mgr_id = protocol_id;
        protocol_to_mgr_[protocol_id] = mgr_id;
        spdlog::debug("Registered energy node {} at ({},{},{})", protocol_id, x, y, z);
    } else {
        mgr_id = it->second;
    }

    NodeState& st = node_states_[mgr_id];
    st.protocol_id = protocol_id;
    st.energy = update->energy();
    st.capacity = update->capacity();
    st.max_input = update->max_input();
    st.max_output = update->max_output();
    st.tier = update->tier();
    st.type = update->energy_type();
    st.is_source = update->is_source();
    st.is_sink = update->is_sink();

    // Wire up CableGraph for ELECTRICITY nodes
    if (st.type == Protocol::EnergyType_ELECTRICITY) {
        if (st.is_source) cable_graph_.registerGenerator(mgr_id, x, y, z);
        if (st.is_sink)   cable_graph_.registerMachine(mgr_id, x, y, z);
    }

    if (update->connected_nodes() && update->connected_nodes()->size() > 0) {
        for (auto it_c = update->connected_nodes()->begin();
             it_c != update->connected_nodes()->end(); ++it_c) {
            uint64_t peer_proto = *it_c;
            auto peer_it = protocol_to_mgr_.find(peer_proto);
            if (peer_it != protocol_to_mgr_.end()) {
                network_manager_.addEdge(mgr_id, peer_it->second);
            }
        }
    }
}

void PipeNetworkService::handleCheckRequest(const std::vector<uint8_t>& data) {
    CheckBridge br{protocol_to_mgr_, network_manager_, node_states_, router_};
    handleCheckTemplate<Protocol::EnergyCheckReq, Protocol::EnergyCheckResp>(
        data, "energy.check.response", br, &Protocol::CreateEnergyCheckResp);
}

void PipeNetworkService::handleConsumeRequest(const std::vector<uint8_t>& data) {
    auto* req = flatbuffers::GetRoot<Protocol::EnergyConsumeReq>(data.data());
    if (!req || !req->pos()) return;
    auto pit = protocol_to_mgr_.find(req->node_id());
    if (pit == protocol_to_mgr_.end()) {
        flatbuffers::FlatBufferBuilder fbb;
        auto resp = Protocol::CreateEnergyConsumeResp(fbb, 0, 0);
        fbb.Finish(resp);
        router_.Publish("energy.consume.response", {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
        return;
    }

    CheckBridge br{protocol_to_mgr_, network_manager_, node_states_, router_};
    uint64_t mgr_id = pit->second;
    int32_t total_source = 0;
    int source_count = 0;
    int32_t consumed = computeConsume(mgr_id, req->amount(), br, total_source, source_count);

    if (consumed > 0 && total_source > 0) {
        int32_t remaining_debt = consumed;
        for (const auto* net : br.network_manager.getAllNetworks()) {
            bool found = false;
            for (uint64_t nid : net->nodeIds) if (nid == mgr_id) { found = true; break; }
            if (!found) continue;
            for (uint64_t nid : net->nodeIds) {
                auto si = br.node_states.find(nid);
                if (si == br.node_states.end() || !si->second.is_source) continue;
                int32_t take = (source_count > 1)
                    ? static_cast<int32_t>(static_cast<int64_t>(consumed) * si->second.energy / total_source)
                    : (std::min)(remaining_debt, si->second.energy);
                take = (std::min)(take, si->second.energy);
                si->second.energy -= take;
                remaining_debt -= take;

                Protocol::Vec3i flowPos(req->pos()->x(), req->pos()->y(), req->pos()->z());
                flatbuffers::FlatBufferBuilder fbb;
                auto event = Protocol::CreateEnergyFlowEvent(
                    fbb, mgr_id, si->second.protocol_id, req->node_id(),
                    static_cast<Protocol::EnergyType>(si->second.type), take, &flowPos, si->second.tier);
                fbb.Finish(event);
                router_.Publish("energy.flow", {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
            }
            break;
        }
    }

    int32_t remaining = 0;
    auto sit = br.node_states.find(mgr_id);
    if (sit != br.node_states.end()) remaining = sit->second.energy;

    flatbuffers::FlatBufferBuilder fbb;
    auto resp = Protocol::CreateEnergyConsumeResp(fbb, consumed, remaining);
    fbb.Finish(resp);
    router_.Publish("energy.consume.response", {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
}

void PipeNetworkService::handleFluidNodeUpdate(const std::vector<uint8_t>& data) {
    auto* update = flatbuffers::GetRoot<Protocol::FluidNodeUpdate>(data.data());
    if (!update || !update->pos()) return;

    uint64_t protocol_id = update->node_id();
    int32_t x = update->pos()->x();
    int32_t y = update->pos()->y();
    int32_t z = update->pos()->z();

    auto it = protocol_to_mgr_.find(protocol_id);
    uint64_t mgr_id;
    if (it == protocol_to_mgr_.end()) {
        if (!network_manager_.addNodeWithId(protocol_id, x, y, z, BLOCK_ID_FLUID_PIPE)) {
            spdlog::warn("Duplicate fluid node {}", protocol_id);
            return;
        }
        mgr_id = protocol_id;
        protocol_to_mgr_[protocol_id] = mgr_id;
        spdlog::debug("Registered fluid node {} at ({},{},{})", protocol_id, x, y, z);
    } else {
        mgr_id = it->second;
    }

    NodeState& st = node_states_[mgr_id];
    st.protocol_id = protocol_id;
    st.energy = update->amount();
    st.capacity = update->capacity();
    st.tier = update->tier();
    st.is_source = update->is_source();
    st.is_sink = update->is_sink();

    if (update->connected_nodes() && update->connected_nodes()->size() > 0) {
        for (auto it_c = update->connected_nodes()->begin();
             it_c != update->connected_nodes()->end(); ++it_c) {
            uint64_t peer_proto = *it_c;
            auto peer_it = protocol_to_mgr_.find(peer_proto);
            if (peer_it != protocol_to_mgr_.end()) {
                network_manager_.addEdge(mgr_id, peer_it->second);
            }
        }
    }
}

void PipeNetworkService::handleFluidCheckRequest(const std::vector<uint8_t>& data) {
    CheckBridge br{protocol_to_mgr_, network_manager_, node_states_, router_};
    handleCheckTemplate<Protocol::FluidCheckReq, Protocol::FluidCheckResp>(
        data, "fluid.check.response", br, &Protocol::CreateFluidCheckResp);
}

void PipeNetworkService::handleFluidConsumeRequest(const std::vector<uint8_t>& data) {
    auto* req = flatbuffers::GetRoot<Protocol::FluidConsumeReq>(data.data());
    if (!req || !req->pos()) return;
    auto pit = protocol_to_mgr_.find(req->node_id());
    if (pit == protocol_to_mgr_.end()) {
        flatbuffers::FlatBufferBuilder fbb;
        auto resp = Protocol::CreateFluidConsumeResp(fbb, 0, 0);
        fbb.Finish(resp);
        router_.Publish("fluid.consume.response", {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
        return;
    }

    CheckBridge br{protocol_to_mgr_, network_manager_, node_states_, router_};
    uint64_t mgr_id = pit->second;
    int32_t total_source = 0;
    int source_count = 0;
    int32_t consumed = computeConsume(mgr_id, req->amount(), br, total_source, source_count);

    if (consumed > 0 && total_source > 0) {
        int32_t remaining_debt = consumed;
        for (const auto* net : br.network_manager.getAllNetworks()) {
            bool found = false;
            for (uint64_t nid : net->nodeIds) if (nid == mgr_id) { found = true; break; }
            if (!found) continue;
            for (uint64_t nid : net->nodeIds) {
                auto si = br.node_states.find(nid);
                if (si == br.node_states.end() || !si->second.is_source) continue;
                int32_t take = (source_count > 1)
                    ? static_cast<int32_t>(static_cast<int64_t>(consumed) * si->second.energy / total_source)
                    : (std::min)(remaining_debt, si->second.energy);
                take = (std::min)(take, si->second.energy);
                si->second.energy -= take;
                remaining_debt -= take;

                flatbuffers::FlatBufferBuilder fbb;
                Protocol::Vec3i flowPos(req->pos()->x(), req->pos()->y(), req->pos()->z());
                auto event = Protocol::CreateFluidFlowEvent(
                    fbb, mgr_id, si->second.protocol_id, req->node_id(),
                    req->fluid_id(), take, &flowPos, si->second.tier);
                fbb.Finish(event);
                router_.Publish("fluid.flow", {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
            }
            break;
        }
    }

    int32_t remaining = 0;
    auto sit = br.node_states.find(mgr_id);
    if (sit != br.node_states.end()) remaining = sit->second.energy;

    flatbuffers::FlatBufferBuilder fbb;
    auto resp = Protocol::CreateFluidConsumeResp(fbb, consumed, remaining);
    fbb.Finish(resp);
    router_.Publish("fluid.consume.response", {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
}

void PipeNetworkService::handleItemNodeUpdate(const std::vector<uint8_t>& data) {
    auto* update = flatbuffers::GetRoot<Protocol::ItemNodeUpdate>(data.data());
    if (!update || !update->pos()) return;

    uint64_t protocol_id = update->node_id();
    int32_t x = update->pos()->x();
    int32_t y = update->pos()->y();
    int32_t z = update->pos()->z();

    uint64_t mgr_id;
    if (auto it = protocol_to_mgr_.find(protocol_id); it == protocol_to_mgr_.end()) {
        if (!network_manager_.addNodeWithId(protocol_id, x, y, z, 0)) {
            spdlog::warn("Duplicate item node {}", protocol_id);
            return;
        }
        mgr_id = protocol_id;
        protocol_to_mgr_[protocol_id] = mgr_id;
        spdlog::debug("Registered item node {} at ({},{},{})", protocol_id, x, y, z);
    } else {
        mgr_id = it->second;
    }

    int32_t cap = update->capacity();
    bool is_source = update->is_source();
    bool is_sink = update->is_sink();
    network_manager_.setNodeItemProps(mgr_id, static_cast<uint8_t>(cap > 0 ? cap : 0), is_source);

    // Item nodes use isItemSource / isItemSink (set via setNodeItemProps), NOT
    // PipeNode::isSource/isSink which are energy-grid flags consumed by
    // distributeEnergy(). Mutating them via const_cast would misclassify this
    // item node as an energy generator/consumer — removed.

    auto* items = update->items();
    if (items) {
        for (size_t i = 0; i < items->size(); ++i) {
            auto* s = items->Get(i);
            if (s->count() > 0 && s->item_id() > 0) {
                network_manager_.addNodeItem(mgr_id, s->item_id(), s->count());
            }
        }
    }

    if (update->connected_nodes() && update->connected_nodes()->size() > 0) {
        for (auto it_c = update->connected_nodes()->begin();
             it_c != update->connected_nodes()->end(); ++it_c) {
            uint64_t peer_proto = *it_c;
            auto peer_it = protocol_to_mgr_.find(peer_proto);
            if (peer_it != protocol_to_mgr_.end()) {
                network_manager_.addEdge(mgr_id, peer_it->second);
            }
        }
    } else {
        static const int dx[6] = {0, 0, 0, 0, -1, 1};
        static const int dy[6] = {-1, 1, 0, 0, 0, 0};
        static const int dz[6] = {0, 0, -1, 1, 0, 0};
        for (int f = 0; f < 6; ++f) {
            uint64_t adjKey = posKey(x + dx[f], y + dy[f], z + dz[f]);
            if (auto pit = pipe_nodes_.find(adjKey); pit != pipe_nodes_.end()) {
                network_manager_.addEdge(mgr_id, pit->second);
            }
        }
    }

    spdlog::debug("handleItemNodeUpdate: node={} at ({},{},{}) source={} sink={} caps={} items={}",
                  protocol_id, x, y, z, is_source, is_sink, cap,
                  items ? static_cast<int>(items->size()) : 0);
}

void PipeNetworkService::handleItemTransferRequest(const std::vector<uint8_t>& data) {
    auto* req = flatbuffers::GetRoot<Protocol::ItemTransferReq>(data.data());
    if (!req || !req->pos()) return;

    auto pit = protocol_to_mgr_.find(req->node_id());
    if (pit == protocol_to_mgr_.end()) {
        flatbuffers::FlatBufferBuilder fbb;
        fbb.Finish(Protocol::CreateItemTransferResp(fbb, 0, req->count()));
        router_.Publish("item.transfer.response",
            {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
        return;
    }

    uint64_t mgr_id = pit->second;
    network_manager_.setNodeItemProps(mgr_id, 0, true);
    network_manager_.addNodeItem(mgr_id, req->item_id(), static_cast<uint8_t>(req->count()));

    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(Protocol::CreateItemTransferResp(fbb, req->count(), 0));
    router_.Publish("item.transfer.response",
        {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});

    spdlog::debug("handleItemTransferRequest: node={} item={} count={} queued for delivery",
                  req->node_id(), req->item_id(), req->count());
}

} // namespace pipe_network
} // namespace gtnh
