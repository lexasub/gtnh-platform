#include "gateway.h"
#include "client_state_generated.h"
#include "gateway_generated.h"
#include "pipe_network_generated.h"
#include "quest_generated.h"
#include "recipe_generated.h"
#include "service_health_generated.h"
#include "common/FlatBuffersVerify.h"
#include "common/ResourceBufferStateCodec.h"

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include <chrono>

#define TRACE_LOG(tid, svc, op, dur_us) \
    spdlog::info("[TRACE tid={}] {} {} {}us", (tid), (svc), (op), (dur_us))

void IoUringGateway::on_router_publish(
    const std::string& topic, std::shared_ptr<std::vector<uint8_t>> data)
{
    const uint8_t* payload = data->data();
    size_t plen = data->size();
    spdlog::debug("Gateway: RX topic='{}' len={}", topic, plen);

    // ── Bulk topics: chunk data and world mutations ─────────────────────
    if (topic == "world.chunk.loaded.compressed") {
        flatbuffers::Verifier v(payload, plen);
        auto chunk = v.VerifyBuffer<Protocol::CompressedChunkData>(nullptr)
            ? flatbuffers::GetRoot<Protocol::CompressedChunkData>(payload)
            : nullptr;
        if (!chunk || !chunk->coord()) {
            spdlog::warn("Gateway: Router: invalid CompressedChunkData FlatBuffer");
            return;
        }
        // Delivered to the sessions that requested this coordinate, and to
        // nobody else. A session that never asked is not served, so two
        // clients cannot be fed each other's worlds.
        const gtnh::net::ChunkCoord coord{chunk->coord()->x(), chunk->coord()->y(),
                                          chunk->coord()->z()};
        std::shared_lock<std::shared_mutex> guard(conn_mutex_);
        auto subscribers = sessions_.peek_chunk_subscribers(coord);
        size_t delivered = 0;
        for (const auto &session : subscribers) {
            // A session whose bulk socket is not paired yet KEEPS its pending
            // entry: consuming it here would drop the chunk with nobody to
            // receive it and no way to ask again — ChunkStore answers a chunk
            // request exactly once. Pairing re-issues the request.
            if (!session->has_bulk()) continue;
            session->bulk->send(GatewayMsg::kCompressedChunkData, payload, plen);
            sessions_.consume_chunk_request(session->session_id, coord);
            ++delivered;
        }
        if (delivered == 0)
            spdlog::debug("Gateway: chunk ({},{},{}) had no deliverable subscriber",
                          coord.x, coord.y, coord.z);
    } else if (topic == "world.blocks.changed") {
        uint32_t tid = 0;
        uint64_t src_player = 0;
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::BlockChangedEvent>(nullptr)) {
            auto ev = flatbuffers::GetRoot<Protocol::BlockChangedEvent>(payload);
            tid = ev->request_id();
            src_player = ev->source_player_id();
        }
        // Everyone but the author sees it: they already applied their own
        // optimistic change and got the authoritative BlockAck.
        auto t0 = std::chrono::steady_clock::now();
        broadcast_bulk(GatewayMsg::kBlockUpdate, payload, plen, src_player);
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        TRACE_LOG(tid, "gateway", src_player ? "relay_block_changed" : "broadcast_block_changed", dur);
    } else if (topic.find("entities.") == 0) {
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::EntitySnapshot>(nullptr))
            broadcast_bulk(GatewayMsg::kEntitySnapshot, payload, plen);
        else
            spdlog::warn("Gateway: Router: invalid EntitySnapshot FlatBuffer");
    } else if (topic == "sim.multiblock.created") {
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::MultiblockCreatedEvent>(nullptr))
            broadcast_ctrl(GatewayMsg::kMultiblockEvent, payload, plen);
        else
            spdlog::warn("Gateway: Router: invalid MultiblockCreatedEvent");
    } else if (topic == "sim.multiblock.destroyed") {
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::MultiblockDestroyedEvent>(nullptr))
            broadcast_ctrl(GatewayMsg::kMultiblockEvent, payload, plen);
        else
            spdlog::warn("Gateway: Router: invalid MultiblockDestroyedEvent");
    }
    // ── Ctrl topics ─────────────────────────────────────────────────────
    //
    // Attribution rule: a payload that names its owner (player_id) or carries
    // a request_id the gateway correlated on the way in is routed to that one
    // session. Payloads with neither are broadcast — that is the honest
    // fallback, and the ones still needing correlation are listed in
    // openspec/changes/add-multiplayer-foundation task 3.2.
    else if (topic == "player.actions.ack") {
        uint32_t request_id = 0;
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::BlockAck>(nullptr))
            request_id = flatbuffers::GetRoot<Protocol::BlockAck>(payload)->request_id();
        // Routed to the session that sent the request (the correlation is
        // recorded on the way in). An ack nobody claims falls back to a
        // broadcast: a client is free to send request_id = 0, which carries no
        // correlation, and dropping those would silently lose its result. The
        // leak is bounded by that — a correlated ack never reaches a client
        // that did not ask.
        auto owners = sessions_.claim_request(request_id);
        if (owners.empty()) {
            spdlog::debug("Gateway: BlockAck request_id={} uncorrelated, broadcast",
                          request_id);
            broadcast_ctrl(GatewayMsg::kBlockAck, payload, plen);
            return;
        }
        for (const auto &session : owners) {
            if (auto conn = session_ctrl_socket(session->session_id))
                conn->send(GatewayMsg::kBlockAck, payload, plen);
        }
    }
    else if (topic == "player.actions.directive")
        broadcast_ctrl(GatewayMsg::kBlockActionDirective, payload, plen);
    else if (topic == "player.inventory.update") {
        uint64_t owner = 0;
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::InventoryUpdate>(nullptr))
            owner = flatbuffers::GetRoot<Protocol::InventoryUpdate>(payload)->player_id();
        if (owner != 0) {
            send_to_player_ctrl(owner, GatewayMsg::kInventoryUpdate, payload, plen);
        } else {
            broadcast_ctrl(GatewayMsg::kInventoryUpdate, payload, plen);
        }
    }
    else if (topic == "world.block_entity.update")
        broadcast_ctrl(GatewayMsg::kBlockEntityUpdate, payload, plen);
    else if (topic == gtnh::common::kTopicResourceBufferState) {
        // Server-authoritative machine/port buffer state → client ctrl.
        // Internal PipeNetwork topics (resource.port.*, resource.drain.*,
        // resource.consume.*) are deliberately NOT subscribed/forwarded.
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::ResourceBufferState>(nullptr))
            broadcast_ctrl(GatewayMsg::kResourceBufferState, payload, plen);
        else
            spdlog::warn("Gateway: Router: invalid ResourceBufferState");
    }
    else if (topic == "sim.craft.response")
        broadcast_ctrl(GatewayMsg::kCraftResponse, payload, plen);
    else if (topic == "sim.workbench.state")
        broadcast_ctrl(GatewayMsg::kGridUpdate, payload, plen);
    // Server-driven recipe query replies (RecipeFrame payload, pass-through)
    else if (topic == "recipe.check.response")
        broadcast_ctrl(GatewayMsg::kRecipeCheckResp, payload, plen);
    else if (topic == "recipe.catalog.response")
        broadcast_ctrl(GatewayMsg::kRecipeCatalogResp, payload, plen);
    else if (topic == "recipe.item.response")
        broadcast_ctrl(GatewayMsg::kRecipeItemResp, payload, plen);
    else if (topic == "recipe.machine.response")
        broadcast_ctrl(GatewayMsg::kRecipeMachineResp, payload, plen);
    else if (topic == "pipe.contents.response") {
        uint64_t owner = 0;
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::PipeContentsResp>(nullptr))
            owner = flatbuffers::GetRoot<Protocol::PipeContentsResp>(payload)->player_id();
        if (owner != 0) {
            send_to_player_ctrl(owner, GatewayMsg::kPipeContentsResp, payload, plen);
        } else {
            broadcast_ctrl(GatewayMsg::kPipeContentsResp, payload, plen);
        }
    }
else if (topic == "player.machine.slot.response")
    broadcast_ctrl(GatewayMsg::kSetMachineSlotResp, payload, plen);
    else if (topic == "player.tool.action.response")
        broadcast_ctrl(GatewayMsg::kToolActionResp, payload, plen);
    else if (topic == "player.position.load") {
        // MetaDB answered a join with the saved position: it belongs to that
        // player, and the chunk request it triggers is that player's too.
        auto pos = gtnh::wire::VerifyAndGetRoot<Protocol::PlayerLeft>(payload, plen);
        if (!pos) return;
        auto session = sessions_.by_player(pos->player_id());
        if (!session) {
            spdlog::debug("Gateway: position restored for player {} with no session",
                          pos->player_id());
            return;
        }
        session->last_x = pos->x();
        session->last_y = pos->y();
        session->last_z = pos->z();
        spdlog::info("Gateway: position restored for player {} ([{}, {}, {}])",
                     session->player_id, session->last_x, session->last_y, session->last_z);
        request_spawn_chunk(session->session_id);
    }
    else if (topic == "quest.progress.updated")
        broadcast_ctrl(GatewayMsg::kQuestProgressUpdate, payload, plen);
    else if (topic == "quest.unlocked")
        broadcast_ctrl(GatewayMsg::kQuestUnlockNotification, payload, plen);
    else if (topic == "quest.completed.notification")
        broadcast_ctrl(GatewayMsg::kQuestCompletedNotification, payload, plen);
    else if (topic == "quest.era.transition")
        broadcast_ctrl(GatewayMsg::kQuestEraTransition, payload, plen);
    else if (topic == "meta_db.quest.get.response")
        broadcast_ctrl(GatewayMsg::kQuestProgressUpdate, payload, plen);
    else if (topic == "quest.exchange.response") {
        uint64_t owner = 0;
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::QuestExchangeResponse>(nullptr))
            owner = flatbuffers::GetRoot<Protocol::QuestExchangeResponse>(payload)->player_id();
        if (owner != 0) {
            send_to_player_ctrl(owner, GatewayMsg::kQuestExchangeResponse, payload, plen);
        } else {
            broadcast_ctrl(GatewayMsg::kQuestExchangeResponse, payload, plen);
        }
    }
    else if (topic == "quest.exchange.cooldown.response") {
        uint64_t owner = 0;
        flatbuffers::Verifier v(payload, plen);
        if (v.VerifyBuffer<Protocol::QuestExchangeCooldown>(nullptr))
            owner = flatbuffers::GetRoot<Protocol::QuestExchangeCooldown>(payload)->player_id();
        if (owner != 0) {
            send_to_player_ctrl(owner, GatewayMsg::kQuestExchangeCooldown, payload, plen);
        } else {
            broadcast_ctrl(GatewayMsg::kQuestExchangeCooldown, payload, plen);
        }
    }
    else if (topic == "player.gamemode.changed")
        broadcast_ctrl(GatewayMsg::kGameModeChange, payload, plen);
    else if (topic == "player.scenario.start.response")
        broadcast_ctrl(GatewayMsg::kStartScenarioResp, payload, plen);
    else if (on_router_message)
        on_router_message(topic, payload, plen);
}
