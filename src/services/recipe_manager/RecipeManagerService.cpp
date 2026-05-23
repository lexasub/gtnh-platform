#include "RecipeManagerService.h"
#include <flatbuffers/flatbuffers.h>
#include "recipe_generated.h"
#include <spdlog/spdlog.h>
#include <asio.hpp>

namespace gtnh {
namespace recipe_manager {

RecipeManagerService::RecipeManagerService(
    gtnh::pipenet::MessageRouterClient& router,
    asio::io_context& io,
    std::shared_ptr<RecipeManager::RecipeManager> recipes)
    : router_(router), io_(io), recipe_timer_(io), recipes_(recipes)
{
}

RecipeManagerService::~RecipeManagerService() {
    asio::error_code ec;
    recipe_timer_.cancel(ec);
}

void RecipeManagerService::Start() {
    router_.OnMessage([this](const std::string& topic, const std::vector<uint8_t>& data) {
        onRouterMessage(topic, data);
    });
    router_.Subscribe("recipe.check");
    router_.Subscribe("recipe.craft");
    router_.Subscribe("recipe.evaluate");
    router_.Subscribe("world.block_entity.update");
    spdlog::info("RecipeManagerService started, subscribed to recipe.check/craft/evaluate/block_entity.update");
}

void RecipeManagerService::onRouterMessage(const std::string& topic, const std::vector<uint8_t>& data) {
    if (topic == "world.block_entity.update") {
        handleBlockEntityUpdate(data);
        return;
    }
    if (topic == "recipe.check" || topic == "recipe.craft" || topic == "recipe.evaluate") {
        flatbuffers::Verifier verifier(data.data(), data.size());
        if (!Protocol::VerifyRecipeFrameBuffer(verifier)) {
            spdlog::warn("RecipeManager: invalid FlatBuffer on '{}'", topic);
            return;
        }
        auto* frame = flatbuffers::GetRoot<Protocol::RecipeFrame>(data.data());
        if (!frame || frame->payload_type() != Protocol::RecipePayload_RecipeMessage) {
            spdlog::warn("RecipeManager: unexpected payload type on '{}'", topic);
            return;
        }
        auto* msg = frame->payload_as_RecipeMessage();
        if (!msg) return;
        uint32_t req_id = msg->req_id();
        std::vector<uint8_t> response;
        if (topic == "recipe.check") {
            auto* req = msg->request_as_CheckRecipeReq();
            if (!req) return;
            response = recipes_->handleCheckRecipeRequest(req, req_id);
        } else if (topic == "recipe.craft") {
            auto* req = msg->request_as_CraftReq();
            if (!req) return;
            response = recipes_->handleCraftRequest(req, req_id);
        } else if (topic == "recipe.evaluate") {
            auto* req = msg->request_as_EvaluateConditionsReq();
            if (!req) return;
            response = recipes_->handleEvaluateConditionsRequest(req, req_id);
        }
        if (!response.empty()) {
            publishResponse(topic + ".response", response);
        }
    }
}

void RecipeManagerService::publishResponse(const std::string& replyTopic, const std::vector<uint8_t>& data) {
    router_.Publish(replyTopic, data);
    spdlog::debug("RecipeManager: published {} ({} bytes)", replyTopic, data.size());
}

void RecipeManagerService::handleBlockEntityUpdate(const std::vector<uint8_t>& data) {
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!verifier.VerifyBuffer<Protocol::BlockEntityUpdate>(nullptr)) {
        return;
    }
    auto* update = flatbuffers::GetRoot<Protocol::BlockEntityUpdate>(data.data());
    if (!update || !update->pos()) return;

    auto* pos = update->pos();
    uint16_t machine_id = update->machine_type();
    if (machine_id == 0) return;

    Pos3Key pos_key{pos->x(), pos->y(), pos->z()};
    if (activeRecipes_.find(pos_key) != activeRecipes_.end()) return;

    auto* input_items = update->input_items();
    if (!input_items || input_items->size() == 0) return;

    std::vector<RecipeManager::ItemStack> items;
    items.reserve(input_items->size());
    for (auto* item : *input_items) {
        items.push_back({item->item_id(), item->count(), item->meta()});
    }

    auto* recipe = recipes_->findRecipeByInputs(machine_id, items);
    if (!recipe) return;

    SPDLOG_DEBUG("Recipe matched: {} machine_id={} at ({},{},{})",
                  recipe->id, machine_id, pos->x(), pos->y(), pos->z());

    // Compute result immediately by calling craft on the current inputs
    auto result_items = recipe->craft(items);

    ActiveRecipe ar;
    ar.machine_id = machine_id;
    ar.recipe_id = recipe->id;
    ar.remaining_ticks = recipe->duration;
    ar.x = pos->x();
    ar.y = pos->y();
    ar.z = pos->z();
    for (const auto& ri : result_items) {
        ar.result_slots.push_back(ri);
    }

    if (recipe->duration == 0) {
        // Instant craft — publish completed right away
        publishRecipeCompleted(pos_key, ar);
    } else {
        // Timed craft — store and start timer
        activeRecipes_[pos_key] = std::move(ar);
        if (activeRecipes_.size() == 1) {
            startTimer();
        }
        SPDLOG_DEBUG("Recipe started: {} duration={} at ({},{},{})",
                      recipe->id, recipe->duration, pos->x(), pos->y(), pos->z());
    }
}

void RecipeManagerService::publishRecipeCompleted(Pos3Key key, const ActiveRecipe& recipe) {
    flatbuffers::FlatBufferBuilder builder(256);

    Protocol::Vec3i pos_vec(recipe.x, recipe.y, recipe.z);

    std::vector<Protocol::ItemStack> result_slots;
    result_slots.reserve(recipe.result_slots.size());
    for (const auto& slot : recipe.result_slots) {
        result_slots.push_back(Protocol::ItemStack(slot.item_id, slot.count, slot.metadata));
    }

    auto completed = Protocol::CreateRecipeCompletedDirect(
        builder, &pos_vec, recipe.machine_id,
        recipe.recipe_id.c_str(), &result_slots);
    builder.Finish(completed);

    std::vector<uint8_t> event_data(builder.GetBufferPointer(),
                                    builder.GetBufferPointer() + builder.GetSize());
    router_.Publish("recipe.completed", event_data);

    activeRecipes_.erase(key);
    SPDLOG_DEBUG("Recipe completed: {} at ({},{},{})", recipe.recipe_id, recipe.x, recipe.y, recipe.z);
}

void RecipeManagerService::startTimer() {
    recipe_timer_.expires_after(std::chrono::milliseconds(kTickIntervalMs));
    recipe_timer_.async_wait([this](const asio::error_code& ec) {
        onTimerTick(ec);
    });
}

void RecipeManagerService::onTimerTick(const asio::error_code& ec) {
    if (ec) return;

    bool any_remaining = false;
    for (auto it = activeRecipes_.begin(); it != activeRecipes_.end(); ) {
        auto& recipe = it->second;
        if (recipe.remaining_ticks > 0) {
            recipe.remaining_ticks--;
        }
        if (recipe.remaining_ticks == 0) {
            Pos3Key key = it->first;
            ActiveRecipe completed = std::move(it->second);
            it = activeRecipes_.erase(it);
            publishRecipeCompleted(key, completed);
        } else {
            ++it;
            any_remaining = true;
        }
    }

    if (any_remaining) {
        startTimer();
    }
}

} // namespace recipe_manager
} // namespace gtnh
