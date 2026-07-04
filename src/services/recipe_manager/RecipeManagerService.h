#pragma once

#include <asio.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Client/MessageRouterClient.h"
#include "core_generated.h"
#include <recipe_manager_lib/RecipeManager.h>

namespace gtnh {
namespace recipe_manager {

struct Pos3Key {
  int32_t x, y, z;
  bool operator==(const Pos3Key &) const = default;
};

struct Pos3Hash {
  size_t operator()(const Pos3Key &k) const noexcept {
    uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(k.x));
    h = h * 0x9e3799b97f4a7c15ULL +
        static_cast<uint64_t>(static_cast<uint32_t>(k.y));
    h = h * 0x9e3799b97f4a7c15ULL +
        static_cast<uint64_t>(static_cast<uint32_t>(k.z));
    return static_cast<size_t>(h);
  }
};

struct ActiveRecipe {
  uint16_t machine_id;
  std::string recipe_id;
  uint32_t remaining_ticks;
  std::vector<RecipeManager::ItemStack> result_slots;
  int32_t x, y, z;
};

class RecipeManagerService {
public:
  RecipeManagerService(gtnh::pipenet::MessageRouterClient &router,
                       asio::io_context &io,
                       std::shared_ptr<RecipeManager::RecipeManager> recipes);
  ~RecipeManagerService();

  void Start();

private:
  void onRouterMessage(const std::string &topic,
                       const std::vector<uint8_t> &data);

  void handleCheckRecipe(const std::vector<uint8_t> &data);
  void handleCraft(const std::vector<uint8_t> &data);
  void handleEvaluateConditions(const std::vector<uint8_t> &data);
  void handleBlockEntityUpdate(const std::vector<uint8_t> &data);

  void publishResponse(const std::string &replyTopic,
                       const std::vector<uint8_t> &data);
  void publishRecipeCompleted(Pos3Key pos_key, const ActiveRecipe &recipe);

  void startTimer();
  void onTimerTick(const asio::error_code &ec);

  gtnh::pipenet::MessageRouterClient &router_;
  asio::io_context &io_;
  asio::steady_timer recipe_timer_;
  std::shared_ptr<RecipeManager::RecipeManager> recipes_;
  std::unordered_map<Pos3Key, ActiveRecipe, Pos3Hash> activeRecipes_;
  static constexpr uint32_t kTickIntervalMs = 50;
};

} // namespace recipe_manager
} // namespace gtnh
