#include "ServerRecipeDB.h"
#include "Network/NetClient.h"

#include <flatbuffers/verifier.h>
#include <spdlog/spdlog.h>

// =========================================================================
//  Catalog
// =========================================================================

void ServerRecipeDB::RequestCatalog() {
    if (!netClient_ || catalogLoaded_ || catalogRequested_) return;
    catalogRequested_ = true;
    netClient_->SendRecipeCatalogReq(nextReqId());
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
    // Dedupe: while a request is in flight, drop extra callers — the response
    // populates the cache and the next frame's query hits it.
    if (itemInFlight_.count(item_id)) return;
    itemInFlight_.insert(item_id);
    uint32_t rid = nextReqId();
    pending_[rid] = Pending{0, item_id, 0, 0, std::move(done)};
    netClient_->SendRecipesForItemReq(item_id, 0 /* both */, rid);
}

void ServerRecipeDB::GetRecipesForMachine(uint16_t machine_id, std::function<void()> done) {
    if (!netClient_) return;
    std::vector<RecipeInfo> cached;
    if (machineCache_.Get(machine_id, &cached)) {
        if (done) done();
        return;
    }
    if (machineInFlight_.count(machine_id)) return;
    machineInFlight_.insert(machine_id);
    uint32_t rid = nextReqId();
    pending_[rid] = Pending{1, 0, machine_id, 0, std::move(done)};
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
    if (gridInFlight_.count(key)) return;
    gridInFlight_.insert(key);
    uint32_t rid = nextReqId();
    pending_[rid] = Pending{2, 0, machine_id, key, [this, key, done]() {
        GridMatch m;
        if (gridCache_.Get(key, &m) && done) done(m.output);
    }};
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
