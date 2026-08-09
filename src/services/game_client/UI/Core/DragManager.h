#pragma once

#include "Common/Inventory.h"
#include <cstdint>
#include <functional>

struct SlotStyle;

/// DragManager — единый source of truth для drag-and-drop в инвентаре,
/// крафт-гриде, машинах. Заменяет дублированную DnD логику в SlotGridComponent
/// и CraftingGrid.
///
/// Состояния: Idle → Holding (после pick up) → Idle (после
/// drop/merge/swap/cancel).
///
/// Использование:
///   1. Вызвать OnSlotActivated при клике на слот
///   2. Вызвать UpdateHover каждый кадр для отслеживания слота под курсором
///   3. Вызвать CancelDrag при ESC
///   4. Вызвать RenderPreview каждый кадр для отрисовки перетаскиваемого
///   предмета
///
/// Network: SetActionCallback для отправки InventoryAction на сервер.
class DragManager {
public:
  /// Результат операции над слотом
  struct ActionResult {
    bool consumed = false;        /// true если клик был обработан DnD
    bool isDraggingAfter = false; /// состояние DnD после операции
    int sourceSlot = -1;          /// откуда взяли предмет
    int targetSlot = -1;          /// куда положили (если applicable)
    ItemStack item;               /// предмет который перемещали
    uint8_t count = 0;            /// количество
  };

  /// Обработать активацию слота (клик).
  /// @param slotIndex  индекс слота в переданном векторе
  /// @param slots      ссылка на вектор слотов (инвентарь/грид)
  /// @param button     0=left, 1=right
  /// @param shift      shift held
  /// @param ctrl       ctrl held (same as shift: quick-move)
  /// @param reportedSlotIndex  индекс для отправки в ActionCallback (по умолчанию = slotIndex)
  ActionResult OnSlotActivated(int slotIndex, std::vector<ItemStack> &slots,
                               int button, bool shift, bool ctrl = false,
                               int reportedSlotIndex = -1);

  /// Отменить текущий драг (ESC). Возвращает предмет в sourceSlot.
  void CancelDrag(std::vector<ItemStack> &slots);

  /// Обновить слот под курсором (вызывать каждый кадр из Render)
  void UpdateHover(int slotIndex);

  /// Правый клик по слоту во время драга (распределить по 1 предмету).
  /// Вызывается при наведении на новый слот с зажатой ПКМ.
  void OnRightDragDistribute(int slotIndex, std::vector<ItemStack> &slots);

  /// Состояние
  bool IsDragging() const { return state_ == State::Holding; }
  const ItemStack &GetHeldItem() const { return heldItem_; }
  int GetSourceSlot() const { return sourceSlot_; }
  int GetHoverSlot() const { return hoverSlot_; }

  /// Отрисовать preview предмета под курсором
  void RenderPreview(const SlotStyle &style);

  /// Удалить перетаскиваемый предмет (Q / right-click outside).
  /// Не возвращает предмет в source-слот — он считается выброшенным.
  void DropHeldItem();

  /// Принудительно сбросить состояние (при InventoryUpdate с сервера)
  void Reset();

  void SetMachineDragContext(BlockPos pos, int slotIdx) {
    machineDragPos_ = pos;
    machineDragSlotIdx_ = slotIdx;
    hasMachineDrag_ = true;
  }
  bool HasMachineDragContext() const { return hasMachineDrag_; }
  BlockPos GetMachineDragPos() const { return machineDragPos_; }
  int GetMachineDragSlotIdx() const { return machineDragSlotIdx_; }
  void ClearMachineDragContext() {
    hasMachineDrag_ = false;
    machineDragSlotIdx_ = -1;
  }

  /// Начать внешний drag (из CraftingGrid или другого не-инвентарного
  /// источника).
  /// @param sourceSlot  глобальный индекс источника (например kGridFlag + idx)
  /// @param item        предмет, который взяли
  void StartExternalDrag(int sourceSlot, const ItemStack &item);

  // ── Network callback ──────────────────────────────────────────────
  // Matches server InventoryActionHandler switch:
  //   0 = MOVE (swap src↔dst), 1 = SPLIT (half stack), 2 = DROP (clear src)
  static constexpr uint8_t kActionMove = 0;
  static constexpr uint8_t kActionSplit = 1;
  static constexpr uint8_t kActionDrop = 2;
  static constexpr uint8_t kActionQuickMove = 3;

  // ── Global slot-source numbering ─────────────────────────────────────
  // Drag sources outside the player inventory (0..99) are identified by
  // disjoint ranges, so a source slot alone tells where the item came from.
  // Invariant: ranges stay disjoint AND < 256 (sourceSlot is uint8_t).
  static constexpr int kChestSlotBase = 50;  // 50-99: chest inventory slots
  static constexpr int kGridSlotBase = 100;  // 100-199: crafting grid slots
  static constexpr int kMachineSlotBase = 200; // 200-219: machine input slots
  static constexpr int kMachineOutputBase = 220; // 220+: machine output slots

  // Вызывается после завершения операции (drop/merge/swap/drop-outside)
  using ActionCallback =
      std::function<void(uint8_t actionType, uint8_t sourceSlot,
                         uint8_t targetSlot, uint8_t count)>;
  void SetActionCallback(ActionCallback cb) { cb_ = std::move(cb); }

  // ── Machine action callback ─────────────────────────────────────────
  using MachineActionCallback =
      std::function<void(uint8_t actionType, uint8_t sourceSlot,
                         uint8_t targetSlot, uint8_t count,
                         BlockPos machinePos)>;
  void SetMachineActionCallback(MachineActionCallback cb) { machineCb_ = std::move(cb); }

  // ── Sync с InventoryState (для совместимости) ─────────────────────
  void SyncTo(InventoryState &inv) const;
  void SyncFrom(const InventoryState &inv);

  /// Notify DragManager when machine slot operations succeed/fail from server
  void OnMachineSlotAck(uint8_t slotIdx, bool success);
  using MachineSlotAckCallback = std::function<void(uint8_t slotIdx, bool success)>;
  void SetMachineSlotAckCallback(MachineSlotAckCallback cb) { machineSlotAckCb_ = std::move(cb); }

private:
  enum class State { Idle, Holding };
  State state_ = State::Idle;

  ItemStack heldItem_;
  int sourceSlot_ = -1;       // vector index (for CancelDrag)
  int reportedSourceSlot_ = -1; // reported index (for ActionCallback)
  int hoverSlot_ = -1;
  ActionCallback cb_;
  MachineActionCallback machineCb_;
  MachineSlotAckCallback machineSlotAckCb_;

  BlockPos machineDragPos_{};
  int machineDragSlotIdx_ = -1;
  bool hasMachineDrag_ = false;
};
