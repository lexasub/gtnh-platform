#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace simcore {

class ActionContext;

// Fluid -> filled-bucket item table, loaded from
// data/registry/fluid_buckets.csv (fluid_id,bucket_id).
//
// This is the ONLY bridge between a fluid in a machine's buffer and a bucket in
// a player's inventory (issue gp-jmlq, design (b)). It is deliberately
// explicit: a fluid with no row has no bucket, and a right-click that cannot
// name a bucket must do nothing rather than invent one.
//
// The `bucket_id` column holds the ITEM NAME, not an id. The name is resolved
// through RecipeManager::ItemRegistry at the moment of the click, so the table
// stays a piece of content and item ids keep coming from the one registry that
// owns them.
//
// Same lifecycle as its sibling BlockDrops: Load() once, setInstance() to
// install a specific table, instance() for the current one.
class FluidBuckets {
public:
  static FluidBuckets* Load(const char* csv_path);

  /// The installed table, default-loaded from kDefaultCsvPath on first use.
  /// Never returns nullptr: a table that cannot be read is an EMPTY table,
  /// which makes every bucket click inert rather than wrong.
  static FluidBuckets* instance();

  static void setInstance(FluidBuckets* table) { instance_ = table; }

  /// The filled-bucket item NAME for this fluid, or nullptr when the fluid has
  /// no row — which means "right-clicking a machine holding this fluid with an
  /// empty bucket does nothing".
  const std::string* Get(uint32_t fluid_id) const;

  size_t size() const { return buckets_.size(); }

  // Production path, relative to the daemon's working directory, exactly as
  // machines.yaml / drops.csv are addressed in src/apps/simcore/main.cpp.
  static constexpr const char* kDefaultCsvPath =
      "src/content/data/registry/fluid_buckets.csv";

private:
  std::unordered_map<uint32_t, std::string> buckets_;
  static FluidBuckets* instance_;
};

// Right-click on a machine: lazily create its ECS entity when the block
// predates this simcore instance, report real energy state, open the window.
// Left-click on a machine flagged interact_on_left (e.g. rotare_generator):
// perform the machine interaction instead of breaking the block.
//
// Right-click while holding an empty_bucket additionally FILLS the bucket from
// the machine's fluid buffer (1000 mB out, the matching filled bucket in
// hand) — but only for a fluid that FluidBuckets has a row for. Every other
// right-click, and every failure on the way, falls through untouched to the
// normal machine interaction.
class MachineInteractHandler {
public:
  bool canHandle(const ActionContext& ctx) const;
  void handle(const ActionContext& ctx) const;
};

} // namespace simcore
