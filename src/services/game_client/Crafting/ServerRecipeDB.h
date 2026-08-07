#pragma once

#include "Common/Inventory.h"
#include "recipe_generated.h"

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class NetClient;

// ── ServerRecipeDB
// ─────────────────────────────────────────────────────────────────────────────
// Server-sourced recipe store. Replaces the old client-hardcoded recipe tables
// (ClientRecipeDB / ClientMachineRecipeDB): the client no longer stores any
// recipes itself — it queries RecipeManagerService through the gateway and
// caches results with LRU eviction on overflow.
//
// Threading: everything runs on the game thread. Incoming responses arrive via
// NetClient::Poll → OnMessage → the callbacks wired in GameClient, which call
// HandleRecipeResponse(). Cache hits invoke callbacks synchronously; misses
// send a request and invoke the callback when the reply lands.
class ServerRecipeDB {
public:
  // Client-side mirror of Protocol::RecipeInfo (recipe.fbs).
  struct RecipeInfo {
    uint16_t machine_type = 0;
    std::string machine_class;
    std::string recipe_id;
    uint32_t duration = 0;
    std::vector<ItemStack> inputs;
    std::vector<ItemStack> outputs;
    bool has_pattern = false;
    std::array<ItemStack, 9> pattern{};
  };

  // Recipes involving one item, split by direction.
  struct ItemRecipes {
    std::vector<RecipeInfo> craft; // recipes producing the item
    std::vector<RecipeInfo> use;   // recipes consuming the item
  };

  // Result of a 3x3 grid check ("what does this grid make").
  struct GridMatch {
    ItemStack output;
    std::string recipe_id;
  };

  ServerRecipeDB() = default;

  void Init(NetClient *netClient) { netClient_ = netClient; }

  // ── Catalog ("what recipes exist") ──────────────────────────────────
  void RequestCatalog();
  bool IsCatalogLoaded() const { return catalogLoaded_; }
  const std::vector<uint16_t> &Catalog() const { return catalog_; }

  // ── Queries (cache-first; async on miss, deduped while in flight) ──
  /// "How is X crafted / where is X used". `done` fires once the item's
  /// recipes are available (immediately on cache hit); read them via
  /// GetItemRecipes().
  void GetRecipesForItem(uint16_t item_id, std::function<void()> done);

  /// "What can machine block M craft" (NEI panel). Read via GetMachineRecipes().
  void GetRecipesForMachine(uint16_t machine_id, std::function<void()> done);

  /// "What does this 3x3 grid make". `done` receives the output item (empty
  /// if no match). Used for the live crafting-table preview.
  void CheckGrid(uint16_t machine_id, const std::array<ItemStack, 9> &grid,
                 std::function<void(const ItemStack &)> done);

  // ── Cached accessors (call from the query callback; empty if not cached) ──
  ItemRecipes GetItemRecipesCopy(uint16_t item_id);
  std::vector<RecipeInfo> GetMachineRecipesCopy(uint16_t machine_id);

  static uint64_t GridKey(uint16_t machine_id,
                          const std::array<ItemStack, 9> &grid);

  // Called by the NetClient recipe callbacks (game thread).
  void HandleRecipeResponse(uint8_t msg_type,
                            std::shared_ptr<std::vector<uint8_t>> data);

private:
  // Bounded LRU cache (most-recently-used on front).
  template <typename K, typename V> class LruCache {
  public:
    explicit LruCache(size_t cap) : cap_(cap) {}
    bool Get(const K &key, V *out) {
      auto it = index_.find(key);
      if (it == index_.end()) return false;
      items_.splice(items_.begin(), items_, it->second);
      *out = it->second->second;
      return true;
    }
    void Put(const K &key, V value) {
      auto it = index_.find(key);
      if (it != index_.end()) {
        it->second->second = std::move(value);
        items_.splice(items_.begin(), items_, it->second);
        return;
      }
      if (items_.size() >= cap_) {
        auto last = items_.end();
        --last;
        index_.erase(last->first);
        items_.pop_back();
      }
      items_.emplace_front(key, std::move(value));
      index_[key] = items_.begin();
    }

  private:
    size_t cap_;
    std::list<std::pair<K, V>> items_;
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator>
        index_;
  };

  // Pending request bookkeeping (key needed to populate the right cache slot).
  struct Pending {
    uint8_t kind; // 0 = item, 1 = machine, 2 = grid
    uint16_t item_id;
    uint16_t machine_id;
    uint64_t grid_key;
    std::function<void()> done;
  };

  uint32_t nextReqId() { return ++nextReqId_; }

  // Response parsers (populate caches, then fire the pending callback).
  void handleCatalogResponse(const Protocol::RecipeReply *reply);
  void handleItemResponse(uint16_t item_id, const Protocol::RecipeReply *reply);
  void handleMachineResponse(uint16_t machine_id,
                             const Protocol::RecipeReply *reply);
  void handleCheckResponse(uint64_t grid_key,
                           const Protocol::RecipeReply *reply);

  static RecipeInfo ParseRecipeInfo(const Protocol::RecipeInfo *info);

  NetClient *netClient_ = nullptr;
  bool catalogLoaded_ = false;
  bool catalogRequested_ = false;
  std::vector<uint16_t> catalog_;

  LruCache<uint16_t, ItemRecipes> itemCache_{256};
  LruCache<uint16_t, std::vector<RecipeInfo>> machineCache_{64};
  LruCache<uint64_t, GridMatch> gridCache_{64};

  uint32_t nextReqId_ = 0;
  std::unordered_map<uint32_t, Pending> pending_;
  std::unordered_set<uint16_t> itemInFlight_;
  std::unordered_set<uint16_t> machineInFlight_;
  std::unordered_set<uint64_t> gridInFlight_;
};
