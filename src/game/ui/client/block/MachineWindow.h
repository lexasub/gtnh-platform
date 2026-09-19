#pragma once

#include <functional>
#include <memory>

#include <game/client/Common/BlockType.h>
#include <game/client/Common/Inventory.h>
#include "../core/DragManager.h"
#include "../BlockAttachedWindow.h"
#include <game/machines/MachineRegistry.h>

class NetClient;
class InputBinder;

// ── MachineWindow — data-driven machine UI ──────────────────────────────────
// Renders input slots, output slots, progress, and energy for a machine
// implementation.  No hardcoded slot layout — reads slot count from the
// interface, so it works with any machine type.
//
// When mech_ is null, renders an empty window (no crash, just stub).
// Mechanism is assigned later via SetMechanism (after construction).
//
// Note: mech_ and related methods are deprecated but kept for backward
// compatibility. Machine type and energy type are now resolved at runtime via
// MachineRegistry.

struct BlockEntityUpdateData {
  float progress = 0.0f;       // Current machine progress (0.0 - 1.0)
  uint32_t energy = 0;         // Current energy stored
  uint32_t energyCapacity = 0; // Maximum energy storage
  EnergyType energyType = EnergyType::ELECTRICITY;
  std::vector<ItemStack> inputItems;
  std::vector<ItemStack> outputItems;
  float heatRatio = 0.0f;      // Heat ratio (0.0 - 1.0+) for overheat warnings
  uint64_t mbId = 0;           // Multiblock ID (0 = not a multiblock)
  // Steam output is a second buffer for heat boilers, while `energy` above
  // remains the primary HU input buffer.
  double steamCurrent = -1.0;
  double steamCapacity = -1.0;
};

// One multiblock hatch shown in the window (task 3.1). `type` matches
// Protocol::HatchType values.
struct HatchRenderData {
  int32_t x = 0, y = 0, z = 0;
  uint8_t type = 0;
  std::vector<ItemStack> items;
};

class MachineWindow : public BlockAttachedWindow {
public:
  MachineWindow(BlockPos pos, uint16_t machineType = 0);
  void SetNetClient(class NetClient *nc) { netClient_ = nc; }
  void SetDragManager(DragManager *dm) { dragMgr_ = dm; }
  void SetBinder(const InputBinder *binder) { binder_ = binder; }
  void SetPlayerId(uint64_t pid) { player_id_ = pid; }
  void SetResourceBufferStore(class ResourceBufferStateStore *store) {
    resourceBuffers_ = store;
  }

  std::string_view Name() const override { return "Machine"; }

  void Render(InventoryState *playerInv) override;
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override;

  // ── Machine type (runtime-resolved via MachineRegistry) ──────────────
  uint16_t GetMachineType() const { return machineType_; }

  // ── Energy type ──────────────────────────────────────────────────────
  EnergyType GetEnergyType() const;
  void SetEnergyType(EnergyType et);

  // Typed resource snapshots are authoritative for machine buffer bars. When
  // one is present, the legacy BlockEntityUpdate energy bar must not be drawn
  // as it would duplicate (for example) the boiler's HU sink.
  static constexpr bool ShouldRenderLegacyEnergyBar(
      bool hasTypedResourceBuffers) noexcept {
    return !hasTypedResourceBuffers;
  }

private:
  bool open_ = false;
  uint16_t machineType_ = 0;
  EnergyType energyType_ = EnergyType::ELECTRICITY;
  DragManager *dragMgr_ = nullptr;
  const InputBinder *binder_ = nullptr;

  // ── Server-authoritative container session (Phase C) ──────────────────
  // machineSlots_ stays unloaded until the container_id=1 InventoryUpdate
  // snapshot arrives; dataLoaded_ gates rendering of server slots.
  uint64_t player_id_ = 0;
  bool dataLoaded_ = false;
  std::vector<ItemStack> machineSlots_;

  // ── Network state ────────────────────────────────────────────────────
  BlockEntityUpdateData pendingUpdate_;
  bool hasPendingUpdate_ = false;
  std::vector<HatchRenderData> pendingHatches_; // multiblock hatches (task 3.1)
  class NetClient *netClient_ = nullptr;

  // ── Recipe completed flash ────────────────────────────────────────────
  float recipeDoneFlash_ = 0.0f;

  // ── Out-of-sync detection ──────────────────────────────────────────
  // Tracks seconds since last viable update. Time-based, not frame-based:
  // a frame threshold scales with refresh rate (30 frames = 0.2s at 144Hz,
  // tripping the stale warning for machines that publish at 0.5s intervals).
  // When the tick channel is healthy this stays well under 0.1s.
  float timeSinceUpdate_ = 0.0f;
  static constexpr float kOutOfSyncSeconds = 0.75f;

  // ── Progress style per machine class ─────────────────────────────────
  enum class ProgressStyle : uint8_t {
    GENERIC,  // flat bar (fallback)
    ARROW,    // furnace / macerator / compressor / extractor / alloy_smelter
    SPINNER,  // mixer / electrolyser / chemical_reactor
    FLAME,    // boiler / generator
  };
  ProgressStyle cachedStyle_ = ProgressStyle::GENERIC;
  bool styleCached_ = false;

  // ── Server-authoritative resource buffers (state store, not raw wire) ──
  class ResourceBufferStateStore *resourceBuffers_ = nullptr;

  ProgressStyle ResolveProgressStyle(const MachineInfo *info);

  // ── Render helpers ──────────────────────────────────────────────────
  void RenderProgress(const MachineInfo *info, float prog);
  void RenderEnergyBarImpl(EnergyType et, uint32_t energy, uint32_t energyMax,
                           float heatRatio = 0.0f, uint64_t mbId = 0);
  void RenderResourceBuffers();
  void RenderOutOfSyncWarning();
};
