#include "PipeNetworkService.h"
#include "Client/MessageRouterClient.h"
#include <common/ResourcePortClient.h>
#include <core_generated.h>
#include <pipe_network_generated.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include "../chunk_store/Storage/cache/MutableChunk.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {

struct SnapshotPipeBlock {
    int32_t x;
    int32_t y;
    int32_t z;
    uint16_t block_id;
    uint8_t meta;
};

// Canonical face order for per-face connection masks.
static constexpr int8_t FACE_DX[6] = {1, -1, 0, 0, 0, 0};
static constexpr int8_t FACE_DY[6] = {0, 0, 1, -1, 0, 0};
static constexpr int8_t FACE_DZ[6] = {0, 0, 0, 0, 1, -1};

} // namespace

namespace gtnh {
namespace pipe_network {

namespace {

// Bridge struct to pass private PipeNetworkService state into generic handlers.
struct CheckBridge {
    std::unordered_map<uint64_t, uint64_t>& protocol_to_mgr;
    pipenet::PipeNetworkManager& network_manager;
    std::unordered_map<uint64_t, gtnh::pipe_network::NodeState>& node_states;
    gtnh::pipe_network::MessageRouterClient& router;
};

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
            if (si != br.node_states.end() && si->second.is_source)
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

int32_t computeConsume(uint64_t mgr_id, int32_t amount,
                       CheckBridge& br, int32_t& out_total_source,
                       int& out_source_count)
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
            if (si != br.node_states.end() && si->second.is_source) {
                out_total_source += si->second.energy;
                ++out_source_count;
            }
        }
        break;
    }
    return consumed;
}

} // anonymous namespace

PipeNetworkService::PipeNetworkService(MessageRouterClient& router, asio::io_context& io)
    : router_(router), io_(io), tick_timer_(io), consume_tracker_(kPendingConsumeTtlTicks),
      next_txn_id_((std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count() | 1)) {}

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
    router_.Subscribe("world.chunk.loaded.compressed");
    router_.Subscribe("world.machine.config.updated");
    router_.Subscribe("pipe.wrench.action");
    router_.Subscribe("pipe.contents.request");
    // Typed resource-port contract (refactor-fluid-port-accounting): typed
    // port registrations/removals feed the manager's port registry, and drain
    // responses complete pending shortfall consumes (3.4.3).
    router_.Subscribe(gtnh::common::kTopicResourcePortRegister);
    router_.Subscribe(gtnh::common::kTopicResourcePortRemove);
    router_.Subscribe(gtnh::common::kTopicResourceDrainResponse);

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
    // Bounded pending map (3.4.3): expire shortfall consumes past their TTL
    // and complete them with the pipe-only amount so consumers never hang on
    // a lost owner response.
    ++service_tick_;

    // 3.5.3: bounded retries with doubling backoff. A retry re-publishes the
    // SAME drain request id — the owner's replay cache answers re-delivery
    // exactly once, so a retry can never double debit — and at most one
    // publish per pending is ever in flight. Before re-publishing, the source
    // port is re-checked against the live registry (3.5.4): a port that was
    // removed or re-registered under a new epoch cancels the pending instead
    // of burning retries against a dead port.
    for (const auto& retry : consume_tracker_.collectRetries(service_tick_)) {
        bool port_live = retry.source_port_id == 0;  // node-based: TTL-bound
        if (!port_live) {
            const auto* port = network_manager_.getPort(
                retry.source_owner_id, gtnh::common::ResourceKind::FLUID,
                retry.source_port_id);
            port_live = port != nullptr && port->epoch == retry.source_epoch;
        }
        if (!port_live) {
            for (const auto& cancelled :
                 consume_tracker_.cancelRequest(retry.drain_request_id)) {
                spdlog::debug(
                    "[PipeNet] shortfall drain {} cancelled at retry time "
                    "(port gone or re-registered); completing short-fill "
                    "(pipe {} of {})",
                    cancelled.drain_request_id, cancelled.pipe_accepted,
                    cancelled.requested);
                publishFluidConsumeResponse(
                    cancelled.pipe_accepted,
                    cancelled.requested - cancelled.pipe_accepted);
            }
            continue;
        }
        const gtnh::common::ResourceTransferRequest drain{
            retry.drain_request_id, retry.source_port_id,
            gtnh::common::ResourceKind::FLUID, retry.fluid_id,
            retry.shortfall};
        router_.Publish(gtnh::common::kTopicResourceDrainRequest,
                        gtnh::common::SerializeDrainRequest(drain));
        spdlog::debug(
            "[PipeNet] shortfall drain {} retry #{} port {} fluid {} amount {}",
            retry.drain_request_id, retry.retry_count, retry.source_port_id,
            retry.fluid_id, retry.shortfall);
    }

    for (const auto& pending : consume_tracker_.expire(service_tick_)) {
        spdlog::debug("[PipeNet] shortfall consume {} expired; completing short-fill "
                      "(pipe {} of {})",
                      pending.drain_request_id, pending.pipe_accepted,
                      pending.requested);
        publishFluidConsumeResponse(pending.pipe_accepted,
                                    pending.requested - pending.pipe_accepted);
    }

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

    // Fluid source buffers are updated authoritatively by fluid.node.update;
    // do not overwrite them from the last snapshot here. fillFluidPipesFromSources
    // debits the manager's transport-side source mirror, and restoring the old
    // snapshot every tick would duplicate fluid indefinitely.
    for (const auto* net : network_manager_.getAllNetworks()) {
        if (!net || net->nodeIds.empty()) continue;
        const auto fluidDeltas = network_manager_.fillFluidPipesFromSources(net->id);
        for (const auto& [node_id, delta] : fluidDeltas) {
            if (delta <= 0) continue;
            const auto* node = network_manager_.getNode(node_id);
            if (!node) continue;
            Protocol::Vec3i pos(node->x, node->y, node->z);
            flatbuffers::FlatBufferBuilder fbb;
            auto update = Protocol::CreateFluidNodeUpdate(
                fbb, node_id, &pos, node->fluidId, node->fluidBuffer,
                node->fluidCapacity, 0, 0, 0, false, false, 0);
            fbb.Finish(update);
            router_.Publish("fluid.pipe.state",
                {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
        }
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
    } else if (topic == "world.chunk.loaded.compressed") {
        handleChunkLoaded(data);
    } else if (topic == "world.machine.config.updated") {
        handleMachineConfigUpdated(data);
    } else if (topic == "pipe.wrench.action") {
        handlePipeWrenchAction(data);
    } else if (topic == "pipe.contents.request") {
        handlePipeContentsRequest(data);
    } else if (topic == gtnh::common::kTopicResourcePortRegister) {
        handleResourcePortRegister(data);
    } else if (topic == gtnh::common::kTopicResourcePortRemove) {
        handleResourcePortRemove(data);
    } else if (topic == gtnh::common::kTopicResourceDrainResponse) {
        handleResourceDrainResponse(data);
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
        for (auto& [chunk_key, positions] : chunk_pipe_positions_) {
            positions.erase(key);
        }
        if (auto cableIt = cable_nodes_.find(key); cableIt != cable_nodes_.end()) {
            network_manager_.removeNode(cableIt->second);
            cable_nodes_.erase(cableIt);
        }
        auto it = pipe_nodes_.find(key);
        if (it != pipe_nodes_.end()) {
            network_manager_.removeNode(it->second);
            // 3.5.3: pendings tied to the removed node (as source or sink)
            // are cancelled — completed short-fill once, marked so a late
            // response drops instead of resurrecting them.
            for (const auto& cancelled :
                 consume_tracker_.cancelForNode(it->second)) {
                spdlog::debug(
                    "[PipeNet] node {} removed; cancelling shortfall drain {} "
                    "short-fill (pipe {} of {})",
                    it->second, cancelled.drain_request_id,
                    cancelled.pipe_accepted, cancelled.requested);
                publishFluidConsumeResponse(
                    cancelled.pipe_accepted,
                    cancelled.requested - cancelled.pipe_accepted);
            }
            pipe_nodes_.erase(it);
            spdlog::debug("[PipeNet] pipe node at ({},{},{}) removed", x, y, z);
        }
        if (auto mit = machine_nodes_.find(key); mit != machine_nodes_.end()) {
            // Machine block gone: its node identity is dead even though the
            // manager node lingers — cancel pendings tied to it (legacy
            // machines have no typed port removal to do it for them).
            for (const auto& cancelled :
                 consume_tracker_.cancelForNode(mit->second)) {
                spdlog::debug(
                    "[PipeNet] machine node {} removed; cancelling shortfall "
                    "drain {} short-fill (pipe {} of {})",
                    mit->second, cancelled.drain_request_id,
                    cancelled.pipe_accepted, cancelled.requested);
                publishFluidConsumeResponse(
                    cancelled.pipe_accepted,
                    cancelled.requested - cancelled.pipe_accepted);
            }
            machine_nodes_.erase(mit);
        }
        pipe_meta_.erase(key);
        cable_graph_.removeCableNode(key);
        return;
    }

    // A non-pipe replacement must remove the old pipe node as well; otherwise
    // the graph retains a stale endpoint until the process restarts.
    if (!isPipeBlock(block_id)) {
        if (auto old = pipe_nodes_.find(key); old != pipe_nodes_.end()) {
            network_manager_.removeNode(old->second);
            pipe_nodes_.erase(old);
            pipe_meta_.erase(key);
        }
        for (auto& [chunk_key, positions] : chunk_pipe_positions_) {
            positions.erase(key);
        }
    }

    // Cable handling must run before the isPipeBlock early-return: cables are
    // not isPipeBlock, so their masks would otherwise never reach CableGraph.
    if (isCableBlock(block_id)) {
        const auto* cableDef = getCableDef(block_id);
        if (cableDef) {
            const uint8_t meta = event->meta();
            if (cable_graph_.hasCableNode(key)) {
                cable_graph_.setCableMeta(key, meta);
            } else {
                cable_graph_.addCableNode(key, *cableDef, x, y, z, meta);
            }
            // Also register a zero-buffer legacy node so machine source/sink
            // updates can discover and connect to cable positions.
            auto cableIt = cable_nodes_.find(key);
            uint64_t nodeId = cableIt == cable_nodes_.end()
                ? network_manager_.addNode(x, y, z, block_id) : cableIt->second;
            cable_nodes_[key] = nodeId;
            pipe_meta_[key] = meta;
            network_manager_.setNodeMeta(nodeId, meta);
            connectEnergyNeighbors(nodeId, x, y, z);
        }
    } else if (auto oldCable = cable_nodes_.find(key); oldCable != cable_nodes_.end()) {
        network_manager_.removeNode(oldCable->second);
        cable_nodes_.erase(oldCable);
    }

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

    const bool isNew = pipe_nodes_.find(key) == pipe_nodes_.end();
    const uint64_t oldNodeId = isNew ? 0 : pipe_nodes_.at(key);
    const auto* oldNode = oldNodeId != 0 ? network_manager_.getNode(oldNodeId) : nullptr;
    const bool blockTypeChanged = oldNode != nullptr && oldNode->block_id != block_id;
    registerPipeBlock(x, y, z, block_id, event->meta());
    const uint64_t nodeId = pipe_nodes_.at(key);
    if (isNew || blockTypeChanged) {
        spdlog::debug("[PipeNet] pipe node {} at ({},{},{}) {}",
                      nodeId, x, y, z, isNew ? "added" : "replaced");
    }

    uint8_t meta = event->meta();
    pipe_meta_[key] = meta;
    network_manager_.setNodeMeta(nodeId, meta);

    // A live block change is authoritative for this position; remember the
    // pipe in its chunk so a later snapshot can reconcile replacements.
    const ChunkKey chunk_key{
        x >= 0 ? x / 32 : (x - 31) / 32,
        y >= 0 ? y / 32 : (y - 31) / 32,
        z >= 0 ? z / 32 : (z - 31) / 32};
    chunk_pipe_positions_[chunk_key].insert(key);
    if (blockTypeChanged) {
        // registerPipeBlock removed the old node and its stale edges. The
        // following refresh rebuilds connections for the replacement type.
    }



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
    std::vector<std::pair<uint64_t, uint64_t>> candidateEdges;
    candidateEdges.reserve(6);
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
                candidateEdges.emplace_back(sourceNodeId, nNode);
            }
        } else {
            // Machine endpoints carry no per-face mask; the pipe side gates.
            if (pipenet::pipeFaceOpen(sourceMeta, f)) {
                candidateEdges.emplace_back(sourceNodeId, nNode);
            }
        }
    }
    // Rebuild once after the full six-face scan. addEdge() intentionally keeps
    // its immediate-rebuild API for existing callers, while this hot path must
    // not rebuild the whole graph once per connected face.
    network_manager_.addEdges(candidateEdges);
}

void PipeNetworkService::handleChunkLoaded(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!verifier.VerifyBuffer<Protocol::CompressedChunkData>()) {
        spdlog::warn("[PipeNet] invalid CompressedChunkData");
        return;
    }
    const auto* event = flatbuffers::GetRoot<Protocol::CompressedChunkData>(data.data());
    if (!event || !event->coord() || !event->palette_data() ||
        event->palette_data()->empty()) {
        spdlog::warn("[PipeNet] CompressedChunkData missing coord or palette");
        return;
    }

    MutableChunk chunk;
    const auto* wire = event->palette_data();
    if (!chunk.fromWire(wire->Data(), wire->size())) {
        spdlog::warn("[PipeNet] invalid chunk wire payload at ({},{},{})",
                     event->coord()->x(), event->coord()->y(), event->coord()->z());
        return;
    }

    const int32_t cx = event->coord()->x();
    const int32_t cy = event->coord()->y();
    const int32_t cz = event->coord()->z();
    std::vector<SnapshotPipeBlock> snapshot;
    for (int ly = 0; ly < 32; ++ly) {
        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                const uint16_t block_id = chunk.getBlock(lx, ly, lz);
                if (!isPipeBlock(block_id)) continue;
                snapshot.push_back({cx * 32 + lx, cy * 32 + ly, cz * 32 + lz,
                                    block_id, chunk.getMeta(lx, ly, lz)});
            }
        }
    }

    const ChunkKey chunk_key{cx, cy, cz};
    auto& previous = chunk_pipe_positions_[chunk_key];
    std::unordered_set<uint64_t> current;
    current.reserve(snapshot.size());
    for (const auto& pipe : snapshot) current.insert(posKey(pipe.x, pipe.y, pipe.z));

    for (const uint64_t old_key : previous) {
        if (current.count(old_key) != 0) continue;
        auto it = pipe_nodes_.find(old_key);
        if (it == pipe_nodes_.end()) continue;
        network_manager_.removeNode(it->second);
        pipe_nodes_.erase(it);
        pipe_meta_.erase(old_key);
    }
    for (const auto& pipe : snapshot) {
        registerPipeBlock(pipe.x, pipe.y, pipe.z, pipe.block_id, pipe.meta);
    }
    previous = std::move(current);
    for (const auto& pipe : snapshot) {
        auto it = pipe_nodes_.find(posKey(pipe.x, pipe.y, pipe.z));
        if (it != pipe_nodes_.end()) {
            refreshPipeConnections(it->second, pipe.x, pipe.y, pipe.z,
                                   pipe.block_id, pipe.meta);
        }
    }
    network_manager_.rebuildItemNetworks();
    spdlog::debug("[PipeNet] hydrated chunk ({},{},{}): {} pipe nodes",
                  cx, cy, cz, snapshot.size());
}

void PipeNetworkService::registerPipeBlock(int32_t x, int32_t y, int32_t z,
                                            uint16_t block_id, uint8_t meta) {
    const uint64_t key = posKey(x, y, z);
    auto it = pipe_nodes_.find(key);
    uint64_t node_id = 0;
    if (it != pipe_nodes_.end()) {
        node_id = it->second;
        const auto* node = network_manager_.getNode(node_id);
        if (!node || node->block_id != block_id) {
            network_manager_.removeNode(node_id);
            pipe_nodes_.erase(it);
            node_id = 0;
        }
    }
    if (node_id == 0) {
        node_id = network_manager_.addNode(x, y, z, block_id);
        pipe_nodes_[key] = node_id;
    }
    pipe_meta_[key] = meta;
    network_manager_.setNodeMeta(node_id, meta);
}

void PipeNetworkService::refreshPipeConnections(uint64_t node_id, int32_t x,
                                                 int32_t y, int32_t z,
                                                 uint16_t block_id, uint8_t meta) {
    network_manager_.removeEdgesForNode(node_id);
    const bool is_item = block_id == BLOCK_ID_ITEM_PIPE ||
                         block_id == BLOCK_ID_DENSE_ITEM_PIPE;
    const bool is_heat = block_id == BLOCK_ID_HEAT_PIPE;
    connectNodeNeighbors(node_id, x, y, z, meta, is_item, is_heat,
                         /*sourceIsPipe=*/true);
}

void PipeNetworkService::connectEnergyNeighbors(uint64_t sourceNodeId,
                                                int32_t x, int32_t y,
                                                int32_t z) {
    constexpr int32_t dx[6] = {1, -1, 0, 0, 0, 0};
    constexpr int32_t dy[6] = {0, 0, 1, -1, 0, 0};
    constexpr int32_t dz[6] = {0, 0, 0, 0, 1, -1};
    for (int i = 0; i < 6; ++i) {
        auto it = cable_nodes_.find(posKey(x + dx[i], y + dy[i], z + dz[i]));
        if (it != cable_nodes_.end()) network_manager_.addEdge(sourceNodeId, it->second);
    }
    for (const auto& [key, node] : machine_nodes_) {
        (void)key;
        const auto* candidate = network_manager_.getNode(node);
        if (!candidate) continue;
        const bool adjacent = std::abs(candidate->x - x) +
                              std::abs(candidate->y - y) +
                              std::abs(candidate->z - z) == 1;
        if (adjacent) network_manager_.addEdge(sourceNodeId, node);
    }
}

bool PipeNetworkService::isPipeBlock(uint16_t block_id) {
    switch (block_id) {
        case BLOCK_ID_ITEM_PIPE:
        case BLOCK_ID_FLUID_PIPE:
        case BLOCK_ID_DENSE_ITEM_PIPE:
        case BLOCK_ID_DENSE_FLUID_PIPE:
        case BLOCK_ID_HEAT_PIPE:
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

void PipeNetworkService::handlePipeContentsRequest(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!verifier.VerifyBuffer<Protocol::PipeContentsReq>()) {
        spdlog::warn("[PipeNet] invalid PipeContentsReq");
        return;
    }

    const auto* req = flatbuffers::GetRoot<Protocol::PipeContentsReq>(data.data());
    if (!req || !req->pos()) {
        spdlog::warn("[PipeNet] PipeContentsReq missing pos");
        return;
    }

    const int32_t x = req->pos()->x();
    const int32_t y = req->pos()->y();
    const int32_t z = req->pos()->z();

    // Read-only lookup: the node is resolved from the pos → node_id maps the
    // service maintains (same source handlePipeWrenchAction uses), and the
    // fluid state is reported exactly as stored in the PipeNode.
    const uint64_t key = posKey(x, y, z);
    uint64_t node_id = 0;
    auto pipeIt = pipe_nodes_.find(key);
    if (pipeIt != pipe_nodes_.end()) {
        node_id = pipeIt->second;
    } else {
        auto machineIt = machine_nodes_.find(key);
        if (machineIt != machine_nodes_.end())
            node_id = machineIt->second;
    }

    // Block-change registration and typed machine updates are asynchronous.
    // During startup the position maps can briefly lag even though the manager
    // already owns the node.  Fall back to the manager's canonical position
    // index, and also recover from a stale service-map entry.
    const auto* node = node_id != 0 ? network_manager_.getNode(node_id) : nullptr;
    if (!node) {
        node_id = network_manager_.findNodeAtPosition(x, y, z);
        node = node_id != 0 ? network_manager_.getNode(node_id) : nullptr;
    }

    bool found = false;
    uint32_t fluid_id = 0;
    int32_t amount = 0;
    int32_t capacity = 0;
    if (node) {
        found = true;
        fluid_id = node->fluidId;
        amount = node->fluidBuffer;
        capacity = node->fluidCapacity;
    }

    flatbuffers::FlatBufferBuilder fbb;
    Protocol::Vec3i pos(x, y, z);
    auto resp = Protocol::CreatePipeContentsResp(
        fbb, req->player_id(), &pos, found, node_id, fluid_id, amount, capacity);
    fbb.Finish(resp);
    router_.Publish("pipe.contents.response",
        {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});

    spdlog::debug("[PipeNet] contents query ({},{},{}) -> found {} node {} fluid {} {}/{}",
                  x, y, z, found, node_id, fluid_id, amount, capacity);
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
            // Chunk hydration allocates local pipe IDs before the simulation
            // publishes machine IDs. Keep the protocol ID as the lookup key,
            // but allocate a collision-free manager ID for this machine.
            mgr_id = network_manager_.findNodeAtPosition(x, y, z);
            const auto* existing = mgr_id != 0 ? network_manager_.getNode(mgr_id) : nullptr;
            if (existing != nullptr && existing->block_id != 1) {
                mgr_id = network_manager_.addNode(x, y, z, 1);
            }
            if (mgr_id == 0) {
                spdlog::warn("Unable to register energy node {}", protocol_id);
                return;
            }
        } else {
            mgr_id = protocol_id;
        }
        protocol_to_mgr_[protocol_id] = mgr_id;
        machine_nodes_[posKey(x, y, z)] = mgr_id;
        spdlog::debug("Registered energy node {} at ({},{},{}) as manager node {}",
                      protocol_id, x, y, z, mgr_id);
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
    } else if (st.type == 0 || st.type == 3) {
        connectEnergyNeighbors(mgr_id, x, y, z);
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
            // A hydrated pipe may already occupy the protocol ID. Reuse an
            // existing machine node at this position, otherwise allocate a
            // separate manager node while retaining the protocol ID.
            mgr_id = network_manager_.findNodeAtPosition(x, y, z);
            const auto* existing = mgr_id != 0 ? network_manager_.getNode(mgr_id) : nullptr;
            if (existing == nullptr) {
                mgr_id = network_manager_.addNode(x, y, z, BLOCK_ID_FLUID_PIPE);
            }
            if (mgr_id == 0) {
                spdlog::warn("Unable to register fluid node {}", protocol_id);
                return;
            }
        } else {
            mgr_id = protocol_id;
        }
        protocol_to_mgr_[protocol_id] = mgr_id;
        machine_nodes_[posKey(x, y, z)] = mgr_id;
        spdlog::debug("Registered fluid node {} at ({},{},{}) as manager node {}",
                      protocol_id, x, y, z, mgr_id);
    } else {
        mgr_id = it->second;
    }

    // Every fluid update is authoritative, including the first update for a
    // machine node created with the fluid-pipe topology default. The default
    // pipe capacity is intentionally non-zero, so gating this call on capacity
    // silently drops the boiler's amount, fluid id, and source role.
    network_manager_.setNodeFluid(mgr_id, update->amount(), update->capacity(),
                                  update->fluid_id(), update->is_source(), update->is_sink());

    NodeState& st = node_states_[mgr_id];
    st.protocol_id = protocol_id;
    st.energy = update->amount();
    st.capacity = update->capacity();
    st.tier = update->tier();
    st.fluid_id = update->fluid_id();
    st.is_source = update->is_source();
    st.is_sink = update->is_sink();
    if (update->max_output() > 0) st.max_output = update->max_output();

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
    if (!req || !req->pos()) {
        spdlog::warn("[PipeNet] invalid FluidConsumeReq");
        return;
    }

    auto pit = protocol_to_mgr_.find(req->node_id());
    if (pit == protocol_to_mgr_.end()) {
        // A request can race the node update on startup.  Position lookup keeps
        // the response routable for ECS entity zero and for that small window.
        auto mit = machine_nodes_.find(posKey(req->pos()->x(), req->pos()->y(), req->pos()->z()));
        if (mit == machine_nodes_.end()) {
            flatbuffers::FlatBufferBuilder fbb;
            auto resp = Protocol::CreateFluidConsumeResp(fbb, 0, req->amount());
            fbb.Finish(resp);
            router_.Publish("fluid.consume.response",
                {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
            return;
        }
        pit = protocol_to_mgr_.emplace(req->node_id(), mit->second).first;
    }

    const uint64_t mgr_id = pit->second;
    const uint32_t fluid_id = req->fluid_id();
    const int32_t requested = req->amount();
    if (requested <= 0 || fluid_id == 0) {
        // 3.4.4: blocked request — zero accepted, exact remaining.
        publishFluidConsumeResponse(0, requested > 0 ? requested : 0);
        return;
    }

    // The manager owns pipe-buffer debits (3.3.1). The checked-in wire request
    // carries no request_id, so the service mints one per delivery: node ids
    // must never double as request ids (3.5.2 — no positional correlation).
    // When FluidConsumeReq grows a request_id field, thread it through here.
    const uint64_t consume_request_id = ++next_txn_id_;
    const auto pipe_result = network_manager_.consumeFluid(
        mgr_id, consume_request_id, fluid_id, requested);
    const int32_t pipe_accepted = pipe_result.accepted_amount;
    const int32_t remaining_demand = pipe_result.remaining;

    // Shortfall: the source is owner state (SimulationCore), never a service
    // mirror (3.3.1/3.3.2). The snapshot below is a routing cache only — it
    // selects whom to ask and is never debited here; the owner is asked with
    // a typed ResourceDrainRequest for exactly the shortfall (3.4.2) and its
    // response completes the consume exactly once (3.4.3).
    if (remaining_demand > 0) {
        const auto network = network_manager_.discoverNetwork(mgr_id);
        // Typed FLUID source ports by position. The drain request carries the
        // resolved port_id; a source without a typed port falls back to a
        // node-based request with port_id 0 (legacy producers — the owner
        // resolves those by its own means until all producers emit typed
        // ports). The port's epoch is bound into the pending (3.5.4) so a
        // later re-registration can invalidate in-flight requests.
        struct SourcePortRef {
            uint64_t owner_id;
            gtnh::common::PortId port_id;
            uint64_t epoch;
        };
        std::unordered_map<uint64_t, SourcePortRef> source_ports;
        for (const auto& port : network_manager_.getRegisteredPorts()) {
            if (port.resource_kind != gtnh::common::ResourceKind::FLUID ||
                port.role != gtnh::common::PortRole::SOURCE) {
                continue;
            }
            source_ports.emplace(
                pipenet::pipePosKey(port.x, port.y, port.z),
                SourcePortRef{port.owner_id, port.port_id, port.epoch});
        }

        uint64_t best_node = 0;
        int32_t best_amount = 0;
        uint64_t best_owner = 0;
        gtnh::common::PortId best_port = 0;
        uint64_t best_epoch = 0;
        for (uint64_t nid : network) {
            auto si = node_states_.find(nid);
            if (si == node_states_.end() || !si->second.is_source) continue;
            const auto* node = network_manager_.getNode(nid);
            if (node && node->fluidId != 0 && node->fluidId != fluid_id) continue;
            if (si->second.energy <= 0) continue;
            uint64_t cand_owner = 0;
            gtnh::common::PortId cand_port = 0;
            uint64_t cand_epoch = 0;
            if (node) {
                if (auto sp = source_ports.find(
                        pipenet::pipePosKey(node->x, node->y, node->z));
                    sp != source_ports.end()) {
                    cand_owner = sp->second.owner_id;
                    cand_port = sp->second.port_id;
                    cand_epoch = sp->second.epoch;
                }
            }
            // 3.5.3: a source inside its rejection cooldown is not probed
            // again — prefer the next-best live source; if every candidate
            // cools down the consume short-fills from the pipe part.
            if (consume_tracker_.sourceInRejectionBackoff(
                    {nid, cand_owner, cand_port}, service_tick_)) {
                continue;
            }
            if (si->second.energy > best_amount) {
                best_amount = si->second.energy;
                best_node = nid;
                best_owner = cand_owner;
                best_port = cand_port;
                best_epoch = cand_epoch;
            }
        }

        if (best_node != 0) {
            const uint64_t drain_request_id = ++next_txn_id_;
            const auto decision = consume_tracker_.planShortfall(
                drain_request_id, consume_request_id, mgr_id, fluid_id,
                requested, pipe_accepted, best_node, best_owner, best_port,
                service_tick_, best_epoch);
            if (decision.request_source) {
                // 3.4.2: one typed drain request for ONLY the shortfall; the
                // response (or expiry/removal) completes the consume — never
                // answered synchronously.
                const gtnh::common::ResourceTransferRequest drain{
                    drain_request_id, best_port,
                    gtnh::common::ResourceKind::FLUID, fluid_id,
                    decision.shortfall};
                router_.Publish(gtnh::common::kTopicResourceDrainRequest,
                                gtnh::common::SerializeDrainRequest(drain));
                spdlog::debug(
                    "[PipeNet] shortfall drain request {} port {} node {} fluid {} "
                    "amount {} (pipe served {} of {})",
                    drain_request_id, best_port, best_node, fluid_id,
                    decision.shortfall, pipe_accepted, requested);
                return;
            }
        }

        // 3.4.4: no source to ask — short-fill with the exact pipe amount.
    }

    publishFluidConsumeResponse(pipe_accepted, remaining_demand);
    spdlog::debug("[PipeNet] fluid consume node={} fluid={} requested={} pipe={} accepted={} remaining={}",
                  req->node_id(), fluid_id, requested, pipe_accepted,
                  pipe_accepted, remaining_demand);
}

void PipeNetworkService::publishFluidConsumeResponse(int32_t consumed,
                                                     int32_t remaining) {
    flatbuffers::FlatBufferBuilder fbb;
    auto resp = Protocol::CreateFluidConsumeResp(fbb, consumed, remaining);
    fbb.Finish(resp);
    router_.Publish("fluid.consume.response",
        {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
}

void PipeNetworkService::handleResourcePortRegister(const std::vector<uint8_t>& data) {
    gtnh::common::ResourcePort port;
    uint32_t resource_id = 0;
    if (!gtnh::common::ParsePortRegister(data.data(), data.size(), &port,
                                         &resource_id)) {
        spdlog::warn("[PipeNet] invalid ResourcePortRegister");
        return;
    }
    const gtnh::common::ResourcePort* existing =
        network_manager_.getPort(port.owner_id, port.resource_kind,
                                 port.port_id);
    const bool replaced = existing != nullptr;
    const uint64_t replaced_epoch = replaced ? existing->epoch : 0;
    if (network_manager_.registerPort(port)) {
        // A successful registration clears the warn-once marker so a future
        // stale epoch for the same port is reported again.
        stale_epoch_warned_.erase({port.owner_id, port.resource_kind,
                                   port.port_id, 0});
        // 3.5.4(c): a successful registration under a NEW epoch redefines the
        // port; pendings bound to the old epoch can never be validly applied
        // (their responses would fail the liveness gate), so they are
        // cancelled now — completed short-fill once, marked so late responses
        // drop. Same-epoch republishes (producers re-register every tick,
        // 2.4.3) leave pendings untouched. Only FLUID registrations matter:
        // pending drains are FLUID-only and port identity includes the kind.
        if (replaced && replaced_epoch != port.epoch &&
            port.resource_kind == gtnh::common::ResourceKind::FLUID) {
            // The incarnation changed: recorded rejection failures no longer
            // describe the current source (3.5.3).
            consume_tracker_.clearRejectionBackoffForPort(port.owner_id,
                                                          port.port_id);
            for (const auto& cancelled : consume_tracker_.cancelStaleEpoch(
                     port.owner_id, port.port_id, port.epoch)) {
                spdlog::debug(
                    "[PipeNet] port {} owner {} re-registered at epoch {} "
                    "(was {}); cancelling drain {} short-fill (pipe {} of {})",
                    port.port_id, port.owner_id, port.epoch, replaced_epoch,
                    cancelled.drain_request_id, cancelled.pipe_accepted,
                    cancelled.requested);
                publishFluidConsumeResponse(
                    cancelled.pipe_accepted,
                    cancelled.requested - cancelled.pipe_accepted);
            }
        }
        return;
    }
    // Manager rejected (stale epoch or invalid port): log once per port, not
    // per republished tick (2.5.3).
    const gtnh::common::ResourcePortRegistrationKey warn_key{
        port.owner_id, port.resource_kind, port.port_id, 0};
    if (stale_epoch_warned_.insert(warn_key).second) {
        spdlog::warn(
            "[PipeNet] port register rejected (stale epoch or invalid port): "
            "owner {} kind {} port {} epoch {}",
            port.owner_id, static_cast<int>(port.resource_kind), port.port_id,
            port.epoch);
    }
}

void PipeNetworkService::handleResourcePortRemove(const std::vector<uint8_t>& data) {
    gtnh::common::ResourcePortRegistrationKey key;
    if (!gtnh::common::ParsePortRemove(data.data(), data.size(), &key)) {
        spdlog::warn("[PipeNet] invalid ResourcePortRemove");
        return;
    }
    // 2.5.1: exact (owner, kind, port, epoch) removal — the epoch is passed
    // through untouched and the manager rejects stale epochs.
    if (!network_manager_.removePort(key.owner_id, key.resource_kind,
                                     key.port_id, key.epoch)) {
        const gtnh::common::ResourcePortRegistrationKey warn_key{
            key.owner_id, key.resource_kind, key.port_id, 0};
        if (stale_epoch_warned_.insert(warn_key).second) {
            spdlog::warn(
                "[PipeNet] port remove rejected (unknown port or stale epoch): "
                "owner {} kind {} port {} epoch {}",
                key.owner_id, static_cast<int>(key.resource_kind),
                key.port_id, key.epoch);
        }
        return;
    }
    // 2.5.3: pending transactions addressed to the removed port complete as
    // zero/short-fill (source part 0) so consumers are never left hanging.
    for (const auto& pending :
         consume_tracker_.clearForPort(key.owner_id, key.port_id)) {
        spdlog::debug(
            "[PipeNet] port {} owner {} removed; completing consume {} short-fill "
            "(pipe {} of {})",
            key.port_id, key.owner_id, pending.drain_request_id,
            pending.pipe_accepted, pending.requested);
        publishFluidConsumeResponse(pending.pipe_accepted,
                                    pending.requested - pending.pipe_accepted);
    }
}

void PipeNetworkService::handleResourceDrainResponse(const std::vector<uint8_t>& data) {
    gtnh::common::ResourceTransferResponse resp;
    if (!gtnh::common::ParseDrainResponse(data.data(), data.size(), &resp)) {
        spdlog::warn("[PipeNet] invalid ResourceDrainResponse");
        return;
    }
    if (resp.resource_kind != gtnh::common::ResourceKind::FLUID) {
        spdlog::debug("[PipeNet] ignoring drain response {} for non-FLUID kind",
                      resp.request_id);
        return;
    }

    // 3.5.4(a)/(c): discard responses whose source port is no longer live at
    // the epoch the request was planned against (removed port, re-registered
    // port, or a missed remove message). The wire response carries no epoch,
    // so the pending's bound epoch is checked against the live registry. The
    // pending is cancelled (marked, completed short-fill once) so retries
    // stop and any further late responses drop. Node-based fallback pendings
    // (port 0) bypass the registry check.
    if (const auto* pending = consume_tracker_.findPending(resp.request_id);
        pending != nullptr && pending->source_port_id != 0) {
        const auto* port = network_manager_.getPort(
            pending->source_owner_id, gtnh::common::ResourceKind::FLUID,
            pending->source_port_id);
        if (!gtnh::pipe_network::drainResponsePortLive(
                *pending, port != nullptr, port ? port->epoch : 0)) {
            for (const auto& cancelled :
                 consume_tracker_.cancelRequest(resp.request_id)) {
                spdlog::debug(
                    "[PipeNet] drain response {} discarded: port {} owner {} "
                    "not live at epoch {} (registered epoch {}); completing "
                    "short-fill (pipe {} of {})",
                    resp.request_id, pending->source_port_id,
                    pending->source_owner_id, pending->source_epoch,
                    port ? port->epoch : 0, cancelled.pipe_accepted,
                    cancelled.requested);
                publishFluidConsumeResponse(
                    cancelled.pipe_accepted,
                    cancelled.requested - cancelled.pipe_accepted);
            }
            return;
        }
    }

    // 3.4.3: apply the accepted amount EXACTLY ONCE — the tracker's replay
    // guard erases applied entries, so duplicate, expired, and mismatched
    // responses are dropped by request id (3.5.2: correlation by id only).
    const auto applied = consume_tracker_.applyDrainResponse(
        resp.request_id, resp.resource_id, resp.accepted_amount);
    if (!applied.applied) {
        spdlog::debug(
            "[PipeNet] drain response {} not applied (unknown/replayed/mismatched)",
            resp.request_id);
        return;
    }
    if (applied.source_accepted != resp.accepted_amount) {
        spdlog::warn(
            "[PipeNet] drain response {} accepted {} clamped to {} (shortfall bound)",
            resp.request_id, resp.accepted_amount, applied.source_accepted);
    }

    // 3.5.3: a zero-accepted answer is a source rejection — cool the source
    // down before the next fresh shortfall probes it again; a productive
    // answer proves it alive and resets any recorded cooldown.
    const gtnh::pipe_network::PipeConsumeTracker::SourceKey source_key{
        applied.completed.source_node_id, applied.completed.source_owner_id,
        applied.completed.source_port_id};
    if (applied.source_accepted == 0) {
        consume_tracker_.noteSourceRejection(source_key, service_tick_);
    } else {
        consume_tracker_.clearSourceRejection(source_key);
    }

    publishFluidFlowTelemetry(applied.completed, applied.source_accepted);
    publishFluidConsumeResponse(applied.combined_accepted, applied.remaining);
    spdlog::debug("[PipeNet] drain response {} accepted {} -> consume combined {} remaining {}",
                  resp.request_id, applied.source_accepted,
                  applied.combined_accepted, applied.remaining);
}

void PipeNetworkService::publishFluidFlowTelemetry(const PendingConsume& pending,
                                                   int32_t amount) {
    if (amount <= 0) return;
    const auto* source_node = network_manager_.getNode(pending.source_node_id);
    if (!source_node) return;
    const auto si = node_states_.find(pending.source_node_id);
    Protocol::Vec3i pos(source_node->x, source_node->y, source_node->z);
    flatbuffers::FlatBufferBuilder fbb;
    // Telemetry only (3.3.3): this fluid.flow event is informational; nothing
    // may debit or credit any owner or pipe buffer through it.
    auto event = Protocol::CreateFluidFlowEvent(
        fbb, pending.source_node_id,
        si != node_states_.end() && si->second.protocol_id != 0
            ? si->second.protocol_id
            : pending.source_node_id,
        pending.sink_node_id, pending.fluid_id, amount, &pos,
        si != node_states_.end() ? si->second.tier : 0);
    fbb.Finish(event);
    router_.Publish("fluid.flow",
        {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()});
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

    // Item roles live in the ITEM resource domain (set via setNodeItemProps),
    // never in the EU-domain roles consumed by distributeEnergy() — the legacy
    // shared is_source/is_sink pair is gone (typed domain model,
    // openspec refactor-fluid-port-accounting 2.3.1).

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
