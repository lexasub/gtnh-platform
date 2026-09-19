#pragma once
#include <cstdint>

namespace simcore {

struct FluidStorage {
  uint32_t fluid_id = 0;
  int32_t amount = 0;
  int32_t capacity = 0;
  int32_t maxInput = 0;
  int32_t maxOutput = 0;

  FluidStorage() = default;

  FluidStorage(uint32_t fid, int32_t amt, int32_t cap,
               int32_t maxIn, int32_t maxOut)
      : fluid_id(fid), amount(amt), capacity(cap),
        maxInput(maxIn), maxOutput(maxOut) {}

  bool isFull() const { return amount >= capacity; }
  bool isEmpty() const { return amount <= 0; }

  int32_t addFluid(int32_t amt) {
    int32_t space = capacity - amount;
    int32_t accepted = (amt < space) ? amt : space;
    if (accepted < 0) accepted = 0;
    if (accepted > maxInput) accepted = maxInput;
    amount += accepted;
    return accepted;
  }

  int32_t removeFluid(int32_t amt) {
    int32_t available = (amount < amt) ? amount : amt;
    if (available > maxOutput) available = maxOutput;
    amount -= available;
    return available;
  }
};

} // namespace simcore
