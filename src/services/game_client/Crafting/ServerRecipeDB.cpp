#include "ServerRecipeDB.h"
#include "Network/NetClient.h"

#include <flatbuffers/verifier.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <ranges>

namespace {
constexpr uint64_t kPendingTimeoutMs = 3000; // 3 s — retry window for lost responses

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

// =========================================================================
//  Catalog
// =========================================================================

void ServerRecipeDB::RequestCatalog() {
    if (!netClient_ || catalogLoaded_ || catalogRequested_) return;
    catalogRequested_ = true;
    netClient_->SendRecipeCatalogReq(nextReqId());
}

void ServerRecipeDB::RetryCatalog() {
    if (!netClient_ || catalogLoaded_) return;
    catalogRequested_ = false;
    RequestCatalog();
}

// =========================================================================
//  Timeout poll
// =========================================================================

void ServerRecipeDB::PollTimeouts() {
    uint64_t now = nowMs();
    // Collect timed-out request ids; mutate pending_ after scanning.
    std::vector<uint32_t> timedOut;
    for (auto& [rid, p] : pending_) {
        if (now - p.sent_at_ms < kPendingTimeoutMs) continue;
        timedOut.push_back(rid);
        // Fire all callbacks with empty results so the UI can retry next query.
        if (p.done) p.done();
        for (auto& cb : p.extra_callbacks)
            if (cb) cb();
        // Clear in-flight flags so future queries for the same key can proceed.
        switch (p.kind) {
        case 0: itemInFlight_.erase(p.item_id); break;
        case 1: machineInFlight_.erase(p.machine_id); break;
        case 2: gridInFlight_.erase(p.grid_key); break;
        }
        spdlog::debug("ServerRecipeDB: request {} timed out (kind={} item={} machine={})",
                      rid, p.kind, p.item_id, p.machine_id);
    }
    for (auto rid : timedOut) pending_.erase(rid);

    // Catalog retry: if the first RequestCatalog was lost (reciped wasn't up yet),
    // reset the sentinel flag and re-request.
    if (catalogRequested_ && !catalogLoaded_) {
        // The catalog doesn't use a Pending entry, so we can't track its age.
        // Just retry on every poll until it loads — cheap extra messages beat
        // a permanently empty NEI panel.
        catalogRequested_ = false;
        RequestCatalog();
    }
}

// =========================================================================
//  Queries (cache-first)
// =========================================================================

void ServerRecipeDB::GetRecipesForItem(uint16_t item_id, std::function<void()> done) {
    if (!netClient_) return;
    ItemRecipes cached;
    if (itemCache_.Get(item_id, &cached)) {
        if (done) done();
        return;
    }
    // While a request is in flight, queue the extra callback instead of
    // dropping it. The primary callback + all queued callbacks are invoked
    // when the response arrives (or on timeout in PollTimeouts).
    if (itemInFlight_.count(item_id)) {
        // Find the pending entry for this item and append the callback.
        for (auto& [rid, p] : pending_) {
            if (p.kind == 0 && p.item_id == item_id) {
                if (done) p.extra_callbacks.push_back(std::move(done));
                return;
            }
        }
        // Stale in-flight flag (shouldn't happen after PollTimeouts is in
        // place, but be defensive): clear it and fall through to a new request.
        itemInFlight_.erase(item_id);
    }
    itemInFlight_.insert(item_id);
    uint32_t rid = nextReqId();
    pending_[rid] = Pending{0, item_id, 0, 0, /*sent_at_ms=*/nowMs(), std::move(done), {}};
    netClient_->SendRecipesForItemReq(item_id, 0 /* both */, rid);
}

void ServerRecipeDB::GetRecipesForMachine(uint16_t machine_id, std::function<void()> done) {
    if (!netClient_) return;
    std::vector<RecipeInfo> cached;
    if (machineCache_.Get(machine_id, &cached)) {
        if (done) done();
        return;
    }
    if (machineInFlight_.contains(machine_id)) {
        for (auto &p: pending_ | std::views::values) {
            if (p.kind == 1 && p.machine_id == machine_id) {
                if (done) p.extra_callbacks.push_back(std::move(done));
                return;
            }
        }
        machineInFlight_.erase(machine_id);
    }
    machineInFlight_.insert(machine_id);
    uint32_t rid = nextReqId();
    pending_[rid] = Pending{1, 0, machine_id, 0, /*sent_at_ms=*/nowMs(), std::move(done), {}};
    netClient_->SendRecipesForMachineReq(machine_id, rid);
}

void ServerRecipeDB::CheckGrid(uint16_t machine_id,
                               const std::array<ItemStack, 9> &grid,
                               std::function<void(const ItemStack &)> done) {
    if (!netClient_) {
        if (done) done(ItemStack{});
        return;
    }
    uint64_t key = GridKey(machine_id, grid);
    GridMatch gm;
    if (gridCache_.Get(key, &gm)) {
        if (done) done(gm.output);
        return;
    }
    if (gridInFlight_.count(key)) {
        for (auto &p: pending_ | std::views::values) {
            if (p.kind == 2 && p.grid_key == key) {
                auto cb = [d = std::move(done), key, this]() mutable {
                    GridMatch m;
                    if (gridCache_.Get(key, &m) && d) d(m.output);
                };
                p.extra_callbacks.push_back(std::move(cb));
                return;
            }
        }
        gridInFlight_.erase(key);
    }
    gridInFlight_.insert(key);
    uint32_t rid = nextReqId();
    // The grid check callback is wrapped because its signature differs from
    // item/machine (takes ItemStack, not void).  The Pending::done still
    // conforms to void() — the unwrapping happens in HandleRecipeResponse.
    pending_[rid] = Pending{.kind = 2, .item_id = 0, .machine_id = machine_id, .grid_key = key, /*sent_at_ms=*/.sent_at_ms = nowMs(),
        .done = [this, key, done]() {
            GridMatch m;
            if (gridCache_.Get(key, &m) && done) done(m.output);
        },
        .extra_callbacks = {}
    };
    netClient_->SendRecipeCheckReq(machine_id, grid, rid);
}

// =========================================================================
//  Cached accessors
// =========================================================================

ServerRecipeDB::ItemRecipes ServerRecipeDB::GetItemRecipesCopy(uint16_t item_id) {
    ItemRecipes out;
    itemCache_.Get(item_id, &out);
    return out;
}

std::vector<ServerRecipeDB::RecipeInfo>
ServerRecipeDB::GetMachineRecipesCopy(uint16_t machine_id) {
    std::vector<RecipeInfo> out;
    machineCache_.Get(machine_id, &out);
    return out;
}

uint64_t ServerRecipeDB::GridKey(uint16_t machine_id,
                                 const std::array<ItemStack, 9> &grid) {
    uint64_t h = machine_id * 0x9e3779b97f4a7c15ULL + 0x517cc1b727220a95ULL;
    for (const auto &s : grid) {
        uint64_t v = (uint64_t(s.item_id) << 16) |
                     (uint64_t(s.meta) & 0xffff) |
                     ((uint64_t(s.count) & 0xff) << 32);
        h = h * 0x9e3779b97f4a7c15ULL ^
            (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    }
    return h;
}

// =========================================================================
//  Response handling (game thread)
// =========================================================================

void ServerRecipeDB::HandleRecipeResponse(
    uint8_t msg_type, std::shared_ptr<std::vector<uint8_t>> data) {
    const uint8_t *payload = data->data();
    size_t plen = data->size();
    flatbuffers::Verifier v(payload, plen);
    if (!Protocol::VerifyRecipeFrameBuffer(v)) {
        spdlog::warn("ServerRecipeDB: invalid RecipeFrame reply (type {})", msg_type);
        return;
    }
    auto *frame = flatbuffers::GetRoot<Protocol::RecipeFrame>(payload);
    if (!frame || frame->payload_type() != Protocol::RecipePayload_RecipeReply) {
        spdlog::warn("ServerRecipeDB: unexpected payload on recipe reply");
        return;
    }
    auto *reply = frame->payload_as_RecipeReply();
    if (!reply) return;

    if (msg_type == GatewayMsg::kRecipeCatalogResp) {
        handleCatalogResponse(reply);
        return;
    }

    auto it = pending_.find(reply->req_id());
    if (it == pending_.end()) return;
    Pending p = std::move(it->second);
    pending_.erase(it);

    switch (p.kind) {
        case 0:
            itemInFlight_.erase(p.item_id);
            handleItemResponse(p.item_id, reply);
            break;
        case 1:
            machineInFlight_.erase(p.machine_id);
            handleMachineResponse(p.machine_id, reply);
            break;
        case 2:
            gridInFlight_.erase(p.grid_key);
            handleCheckResponse(p.grid_key, reply);
            break;
        default:
            return;
    }
    if (p.done) p.done();
    for (auto& cb : p.extra_callbacks)
        if (cb) cb();
}

void ServerRecipeDB::handleCatalogResponse(const Protocol::RecipeReply *reply) {
    auto *resp = reply->response_as_RecipeCatalogResp();
    if (!resp) return;
    catalog_.clear();
    if (auto *ids = resp->item_ids()) {
        catalog_.assign(ids->begin(), ids->end());
    }
    catalogLoaded_ = true;
    catalogRequested_ = false;
    spdlog::debug("ServerRecipeDB: catalog loaded ({} item ids)", catalog_.size());
}

ServerRecipeDB::RecipeInfo ServerRecipeDB::ParseRecipeInfo(
    const Protocol::RecipeInfo *info) {
    RecipeInfo ri;
    if (!info) return ri;
    ri.machine_type = info->machine_type();
    ri.machine_class = info->machine_class() ? info->machine_class()->str() : "";
    ri.recipe_id = info->recipe_id() ? info->recipe_id()->str() : "";
    ri.duration = info->duration();
    ri.unlock_era = info->unlock_era();
    ri.has_pattern = info->has_pattern();
    if (auto *inputs = info->inputs()) {
        ri.inputs.reserve(inputs->size());
        for (flatbuffers::uoffset_t i = 0; i < inputs->size(); ++i) {
            auto *s = inputs->Get(i);
            if (s) ri.inputs.push_back(ItemStack{s->item_id(), s->count(), s->meta()});
        }
    }
    if (auto *outputs = info->outputs()) {
        ri.outputs.reserve(outputs->size());
        for (flatbuffers::uoffset_t i = 0; i < outputs->size(); ++i) {
            auto *s = outputs->Get(i);
            if (s) ri.outputs.push_back(ItemStack{s->item_id(), s->count(), s->meta()});
        }
    }
    if (auto *pattern = info->pattern()) {
        for (flatbuffers::uoffset_t i = 0; i < 9 && i < pattern->size(); ++i) {
            auto *s = pattern->Get(i);
            if (s) ri.pattern[i] = ItemStack{s->item_id(), s->count(), s->meta()};
        }
    }
    return ri;
}

void ServerRecipeDB::handleItemResponse(uint16_t item_id,
                                        const Protocol::RecipeReply *reply) {
    auto *resp = reply->response_as_RecipesForItemResp();
    if (!resp) return;
    ItemRecipes out;
    if (auto *recipes = resp->recipes()) {
        for (flatbuffers::uoffset_t i = 0; i < recipes->size(); ++i) {
            auto *info = recipes->Get(i);
            if (!info) continue;
            RecipeInfo ri = ParseRecipeInfo(info);
            // A recipe can both produce AND consume the same item (e.g.
            // recycling) — it belongs in both tabs.
            bool produces = false, consumes = false;
            for (const auto &o : ri.outputs) {
                if (o.item_id == item_id) { produces = true; break; }
            }
            for (const auto &in : ri.inputs) {
                if (in.item_id == item_id) { consumes = true; break; }
            }
            if (produces) out.craft.push_back(ri);
            if (consumes) out.use.push_back(ri);
        }
    }
    itemCache_.Put(item_id, std::move(out));
}

void ServerRecipeDB::handleMachineResponse(uint16_t machine_id,
                                           const Protocol::RecipeReply *reply) {
    auto *resp = reply->response_as_RecipesForMachineResp();
    if (!resp) return;
    std::vector<RecipeInfo> recipes;
    if (auto *items = resp->recipes()) {
        recipes.reserve(items->size());
        for (flatbuffers::uoffset_t i = 0; i < items->size(); ++i) {
            recipes.push_back(ParseRecipeInfo(items->Get(i)));
        }
    }
    machineCache_.Put(machine_id, std::move(recipes));
}

void ServerRecipeDB::handleCheckResponse(uint64_t grid_key,
                                         const Protocol::RecipeReply *reply) {
    auto *resp = reply->response_as_CheckRecipeResp();
    if (!resp) return;
    GridMatch gm;
    if (resp->recipe_id()) gm.recipe_id = resp->recipe_id()->str();
    if (auto *info = resp->recipe()) {
        if (auto *outputs = info->outputs(); outputs && outputs->size() > 0) {
            auto *o = outputs->Get(0);
            if (o) gm.output = ItemStack{o->item_id(), o->count(), o->meta()};
        }
    }
    gridCache_.Put(grid_key, std::move(gm));
}
