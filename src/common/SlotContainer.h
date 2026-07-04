#pragma once

#include <array>
#include <cstdint>
#include <functional>

// Lightweight item stack (matches RecipeManager::ItemStack and client
// ItemStack). Kept local to avoid dragging in large headers.
struct SlotItem {
  uint16_t item_id = 0;
  uint8_t count = 0;
  uint16_t meta = 0;
  explicit operator bool() const { return item_id != 0; }
};

// Fixed-size slot container with change notification.
// T is the slot value type (defaults to SlotItem).
// On every mutation that actually changes a slot, on_change fires.
template <size_t N, typename T = SlotItem> class SlotContainer {
public:
  using ChangeFn = std::function<void(int index, T oldValue, T newValue)>;

  // ── Read ────────────────────────────────────────────────────────────
  const T &operator[](int i) const { return slots_[i]; }
  const T &at(int i) const { return slots_[i]; }
  const std::array<T, N> &data() const { return slots_; }

  // ── Write (fires on_change) ─────────────────────────────────────────
  void set(int i, T v) {
    if (i < 0 || i >= static_cast<int>(N))
      return;
    T old = slots_[i];
    slots_[i] = v;
    if (on_change_)
      on_change_(i, old, v);
  }

  void setAll(const std::array<T, N> &src) {
    for (size_t i = 0; i < N; ++i)
      set(static_cast<int>(i), src[i]);
  }

  void fill(const T &v) {
    for (size_t i = 0; i < N; ++i)
      set(static_cast<int>(i), v);
  }

  // ── Batch operations ────────────────────────────────────────────────
  void clear() { fill(T{}); }

  // Decrement slot count by `amount`; zeroes the slot if count reaches 0.
  void consume(int i, uint8_t amount = 1) {
    if (i < 0 || i >= static_cast<int>(N))
      return;
    auto s = slots_[i];
    if (s.count <= amount) {
      set(i, T{});
    } else {
      s.count -= amount;
      set(i, s);
    }
  }

  // ── Queries ─────────────────────────────────────────────────────────
  size_t size() const { return N; }
  bool empty(int i) const {
    return i >= 0 && i < static_cast<int>(N) && slots_[i].item_id == 0;
  }

  // ── Iteration ───────────────────────────────────────────────────────
  auto begin() { return slots_.begin(); }
  auto end() { return slots_.end(); }
  auto begin() const { return slots_.begin(); }
  auto end() const { return slots_.end(); }

  // ── Event ───────────────────────────────────────────────────────────
  void setOnChange(ChangeFn fn) { on_change_ = std::move(fn); }

private:
  std::array<T, N> slots_{};
  ChangeFn on_change_;
};
