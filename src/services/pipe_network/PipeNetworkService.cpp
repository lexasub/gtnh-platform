#include "PipeNetworkService.h"
#include "Client/MessageRouterClient.h"
#include <core_generated.h>
#include <pipe_network_generated.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>

namespace {

// Bridge struct to pass private PipeNetworkService state into generic handlers
struct CheckBridge {
    std::unordered_map<uint64_t, uint64_t>& protocol_to_mgr;
    pipenet::PipeNetworkManager& network_manager;
    std::unordered_map<uint64_t, gtnh::pipe_network::NodeState>& node_states;
    gtnh::pipe_network::MessageRouterClient& router;
};

// Canonical face order for per-face connection masks: index = meta bit.
//   0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z  (matches CableGraph / meta-bit convention).
// meta == 0 ⇒ all six faces connected (0x3F).
static constexpr int8_t FACE_DX[6] = { 1, -1, 0, 0, 0, 0};
static constexpr int8_t FACE_DY[6] = { 0,  0, 1,-1, 0, 0};
static constexpr int8_t FACE_DZ[6] = { 0,  0, 0, 0, 1,-1};

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
    router_.Subscribe("world.machine.config.updated");
    router_.Subscribe("pipe.wrench.action");

    loadPersistentState();
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

        network_manager_.distributeHeat(net->id, pipenet::HeatConstants::MAX_HEAT_PER_TICK);
    }
    
    // Interval save of item buffers (TODO research: proper chunk unload hook)
    ++tick_counter_;
    if (tick_counter_ >= PERSIST_INTERVAL_TICKS) {
        tick_counter_ = 0;
        auto buffers = network_manager_.exportItemBuffers();
        if (!buffers.empty()) {
            // Simple file-based persistence: serialize each node's items
            // Format: nodeId:itemId,count;itemId,count|nodeId:...
            std::ostringstream oss;
            for (const auto& [nid, items] : buffers) {
                oss << nid << ":";
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << items[i].item_id << "," << (int)items[i].count;
                }
                oss << "|";
            }
            std::string data = oss.str();
            std::string path = std::string(PERSIST_DIR) + "item_buffers.txt";
            std::ofstream ofs(path, std::ios::trunc);
            if (ofs) {
                ofs << data;
                ofs.close();
                spdlog::trace("[PipeNet] saved {} pipe nodes with items in transit ({} bytes)",
                              buffers.size(), data.size());
            } else {
                spdlog::warn("[PipeNet] failed to save item buffers to {}", path);
            }
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
    } else if (topic == "world.machine.config.updated") {
        handleMachineConfigUpdated(data);
    } else if (topic == "pipe.wrench.action") {
        handlePipeWrenchAction(data);
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
        machine_nodes_.erase(key);
        pipe_meta_.erase(key);
        cable_graph_.removeCableNode(key);
        return;
    }

    // Cable handling must run before the isPipeBlock early-return: cables are
    // not isPipeBlock, so their masks would otherwise never reach CableGraph.
    if (isCableBlock(block_id)) {
        const auto* cableDef = getCableDef(block_id);
        if (cableDef) {
            uint8_t meta = event->meta();
            pipe_meta_[key] = meta;
            if (cable_graph_.hasCableNode(key)) {
                // Wrench toggle / re-place on same key: update mask + rebuild
                // so the per-face connection change takes effect immediately.
                cable_graph_.setCableMeta(key, meta);
            } else {
                cable_graph_.addCableNode(key, *cableDef, x, y, z, meta);
            }
            spdlog::debug("[PipeNet] cable node {} at ({},{},{}) meta={:#x}",
                          key, x, y, z, meta);
        }
    }

    if (!isPipeBlock(block_id)) return;

    bool isNew = (pipe_nodes_.find(key) == pipe_nodes_.end());
    uint64_t nodeId;
    if (isNew) {
        nodeId = network_manager_.addNode(x, y, z, block_id);
        pipe_nodes_.emplace(key, nodeId);
        spdlog::debug("[PipeNet] pipe node {} at ({},{},{}) added", nodeId, x, y, z);
    } else {
        nodeId = pipe_nodes_[key];
    }

    uint8_t meta = event->meta();
    pipe_meta_[key] = meta;
    network_manager_.setNodeMeta(nodeId, meta);

    // Rebuild mask-aware connectivity: drop stale edges, then re-add only the
    // faces both endpoints permit open.
    network_manager_.removeEdgesForNode(nodeId);
    bool isItem = (block_id == BLOCK_ID_ITEM_PIPE || block_id == BLOCK_ID_DENSE_ITEM_PIPE);
    bool isHeat = (block_id == BLOCK_ID_HEAT_PIPE);
    connectNodeNeighbors(nodeId, x, y, z, meta, isItem, isHeat, /*sourceIsPipe=*/true);

    network_manager_.rebuildItemNetworks();
}

void PipeNetworkService::connectNodeNeighbors(uint64_t sourceNodeId,
                                            int32_t x, int32_t y, int32_t z,
                                            uint8_t sourceMeta, bool isItem,
                                            bool isHeat, bool sourceIsPipe) {
    for (int f = 0; f < 6; ++f) {
        int32_t nx = x + FACE_DX[f];
        int32_t ny = y + FACE_DY[f];
        int32_t nz = z + FACE_DZ[f];
        uint64_t nKey = posKey(nx, ny, nz);

        uint64_t nNode = 0;
        bool nIsPipe = false;
        auto pit = pipe_nodes_.find(nKey);
        if (pit != pipe_nodes_.end()) {
            nNode = pit->second;
            nIsPipe = true;
        } else {
            auto mit = machine_nodes_.find(nKey);
            if (mit != machine_nodes_.end()) nNode = mit->second;
        }
        if (nNode == 0) continue;
        // Only connect across exactly one pipe endpoint (pipe↔pipe or pipe↔machine);
        // never machine↔machine.
        if (!sourceIsPipe && !nIsPipe) continue;

        const auto* nn = network_manager_.getNode(nNode);
        if (!nn) continue;
        bool compatible;
        if (isItem) {
            compatible = nn->itemCapacity > 0;
        } else if (isHeat) {
            compatible = nn->heatCapacity > 0;
            // Machine endpoints in heat mode must actually be HEAT-type nodes.
            // Steam machines also carry heatCapacity via setNodeHeat(); without
            // this check a heat pipe would link to a steam-only machine.
            if (compatible && !nIsPipe) {
                auto sit = node_states_.find(nNode);
                if (sit == node_states_.end() ||
                    sit->second.type != Protocol::EnergyType_HEAT) {
                    compatible = false;
                }
            }
        } else {
            compatible = nn->fluidCapacity > 0;
        }
        if (!compatible) continue;

        if (nIsPipe) {
            uint8_t nMeta = pipe_meta_.count(nKey) ? pipe_meta_[nKey] : 0;
            // Connected iff both endpoints open their shared face.
            if (pipenet::pipeFacesConnected(sourceMeta, nMeta, f)) {
                network_manager_.addEdge(sourceNodeId, nNode);
            }
        } else {
            // Machine endpoints carry no per-face mask; the pipe side gates.
            if (pipenet::pipeFaceOpen(sourceMeta, f)) {
                network_manager_.addEdge(sourceNodeId, nNode);
            }
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
    return pipenet::pipePosKey(x, y, z);
}

void PipeNetworkService::handlePipeWrenchAction(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!verifier.VerifyBuffer<Protocol::PipeWrenchAction>()) {
        spdlog::warn("[PipeNet] invalid PipeWrenchAction");
        return;
    }

    const auto* req = flatbuffers::GetRoot<Protocol::PipeWrenchAction>(data.data());
    if (!req || !req->pos()) {
        spdlog::warn("[PipeNet] PipeWrenchAction missing pos");
        return;
    }

    int32_t x = req->pos()->x();
    int32_t y = req->pos()->y();
    int32_t z = req->pos()->z();

    uint64_t node_id = 0;
    auto guidance = pipenet::evaluatePipeWrench(
        pipe_nodes_, machine_nodes_, x, y, z, &node_id);

    uint32_t component_size = 0;
    if (node_id != 0) {
        auto component = network_manager_.discoverNetwork(node_id);
        component_size = static_cast<uint32_t>(component.size());
    }

    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(x, y, z);
    auto resp = Protocol::CreatePipeWrenchResp(
        fbb, req->player_id(), &pos,
        static_cast<Protocol::PipeWrenchGuidance>(guidance), node_id, component_size);
    fbb.Finish(resp);
    router_.Publish("pipe.wrench.response",
        {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});

    spdlog::debug("[PipeNet] wrench on ({},{},{}) -> guidance {} node {} component {}",
                  x, y, z, static_cast<int>(guidance), node_id, component_size);
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
        machine_nodes_[posKey(x, y, z)] = mgr_id;
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

    // Wire up CableGraph for ELECTRICITY / ROTATION nodes
    if (st.type == Protocol::EnergyType_ELECTRICITY || st.type == Protocol::EnergyType_ROTATION) {
        if (st.is_source) cable_graph_.registerGenerator(mgr_id, x, y, z, st.tier);
        if (st.is_sink)   cable_graph_.registerMachine(mgr_id, x, y, z, st.tier);
    } else if (st.type == Protocol::EnergyType_HEAT || st.type == Protocol::EnergyType_STEAM) {
        network_manager_.setNodeHeat(mgr_id, st.energy, st.capacity, st.is_source, st.is_sink);
    }

    // Machine placed after its pipe: build mask-aware edges to neighbours now.
    // HEAT machines link to heat pipes (isHeat=true); the neighbour check in
    // connectNodeNeighbors requires the peer to be a HEAT-type node.
    if (st.type == Protocol::EnergyType_HEAT) {
        connectNodeNeighbors(mgr_id, x, y, z,
                             /*sourceMeta=*/0, /*isItem=*/false, /*isHeat=*/true,
                             /*sourceIsPipe=*/false);
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
        machine_nodes_[posKey(x, y, z)] = mgr_id;
        spdlog::debug("Registered fluid node {} at ({},{},{})", protocol_id, x, y, z);
    } else {
        mgr_id = it->second;
        // Fluid-capacity upgrade: machines register via handleNodeUpdate as
        // energy nodes (blockId=1, fluidCapacity=0). First fluid update lifts
        // them into the fluid layer so the masked scan below forms pipe edges.
        const auto* nn = network_manager_.getNode(mgr_id);
        if (nn && nn->fluidCapacity <= 0) {
            network_manager_.setNodeFluid(mgr_id, update->amount(), update->capacity(),
                                          update->fluid_id(), update->is_source(), update->is_sink());
        }
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

    // Fluid machines previously built no edges (connected_nodes is empty), so
    // fluid never flowed. Add masked machine→fluid-pipe connections: the machine
    // has no per-face mask, the pipe's open faces gate the link.
    connectNodeNeighbors(mgr_id, x, y, z,
                         /*sourceMeta=*/0, /*isItem=*/false, /*isHeat=*/false,
                         /*sourceIsPipe=*/false);
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
        machine_nodes_[posKey(x, y, z)] = mgr_id;
        spdlog::debug("Registered item node {} at ({},{},{})", protocol_id, x, y, z);
    } else {
        mgr_id = it->second;
    }

    int32_t cap = update->capacity();
    bool is_source = update->is_source();
    bool is_sink = update->is_sink();
    network_manager_.setNodeItemProps(mgr_id, static_cast<uint8_t>(cap > 0 ? cap : 0), is_source, is_sink);

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
        // Mask-aware machine→item-pipe connections. Machine endpoints carry no
        // per-face mask; only the pipe's open faces gate the connection, so a
        // wrench-disconnected pipe face no longer links to an adjacent machine.
        connectNodeNeighbors(mgr_id, x, y, z,
                             /*sourceMeta=*/0, /*isItem=*/true, /*isHeat=*/false,
                             /*sourceIsPipe=*/false);
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
    network_manager_.setNodeItemProps(mgr_id, 0, true, false);
    network_manager_.addNodeItem(mgr_id, req->item_id(), static_cast<uint8_t>(req->count()));

    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(Protocol::CreateItemTransferResp(fbb, req->count(), 0));
    router_.Publish("item.transfer.response",
        {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});

    spdlog::debug("handleItemTransferRequest: node={} item={} count={} queued for delivery",
                  req->node_id(), req->item_id(), req->count());
}

void PipeNetworkService::handleMachineConfigUpdated(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!verifier.VerifyBuffer<Protocol::MachineConfigUpdated>()) {
        spdlog::warn("[PipeNet] invalid MachineConfigUpdated");
        return;
    }

    const auto* event = flatbuffers::GetRoot<Protocol::MachineConfigUpdated>(data.data());
    auto* pos = event->pos();
    if (!pos || !event->faces()) return;

    uint64_t key = posKey(pos->x(), pos->y(), pos->z());
    auto pit = pipe_nodes_.find(key);
    if (pit == pipe_nodes_.end()) return;

    uint64_t mgr_id = pit->second;
    std::array<uint8_t, 6> side_config;
    auto* faces = event->faces();
    for (int i = 0; i < 6 && i < static_cast<int>(faces->size()); ++i) {
        side_config[i] = faces->Get(i);
    }
    network_manager_.setNodeSideConfig(mgr_id, side_config);

    spdlog::debug("[PipeNet] side_config updated at ({},{},{}) via wrench: {}{}{}{}{}{}",
                  pos->x(), pos->y(), pos->z(),
                  (int)side_config[0], (int)side_config[1], (int)side_config[2],
                  (int)side_config[3], (int)side_config[4], (int)side_config[5]);
}

void PipeNetworkService::loadPersistentState() {
    std::string path = std::string(PERSIST_DIR) + "item_buffers.txt";
    std::ifstream ifs(path);
    if (!ifs) return;

    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    if (data.empty()) return;

    std::unordered_map<uint64_t, std::vector<pipenet::ItemSlot>> buffers;
    std::istringstream iss(data);
    std::string segment;
    while (std::getline(iss, segment, '|')) {
        if (segment.empty()) continue;
        auto colonPos = segment.find(':');
        if (colonPos == std::string::npos) continue;

        uint64_t nid = std::stoull(segment.substr(0, colonPos));
        std::string itemsStr = segment.substr(colonPos + 1);
        if (itemsStr.empty()) continue;

        std::vector<pipenet::ItemSlot> items;
        std::istringstream itemIss(itemsStr);
        std::string itemSeg;
        while (std::getline(itemIss, itemSeg, ',')) {
            auto commaPos = itemSeg.find(',');
            if (commaPos == std::string::npos || commaPos == 0) continue;
            uint16_t itemId = static_cast<uint16_t>(std::stoul(itemSeg.substr(0, commaPos)));
            uint8_t count = static_cast<uint8_t>(std::stoul(itemSeg.substr(commaPos + 1)));
            items.push_back({itemId, count});
        }

        if (!items.empty()) {
            buffers[nid] = items;
        }
    }

    if (!buffers.empty()) {
        network_manager_.importItemBuffers(buffers);
        spdlog::info("[PipeNet] restored {} pipe nodes with items in transit from persistent state",
                     buffers.size());
        std::remove(path.c_str());
    }
}

} // namespace pipe_network
} // namespace gtnh
