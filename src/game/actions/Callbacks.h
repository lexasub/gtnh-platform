#pragma once
#include <cstdint>
#include <functional>

namespace simcore {

// Player inventory grant: (player_id, item_id, count, target_slot).
using ItemGiveCallback = std::function<void(
    uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot)>;

// Drill energy consumption hook after a block is broken.
using DrillUseCallback = std::function<void(
    uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id)>;

// Quest hook after a block is placed.
using BlockPlacedCallback = std::function<void(
    uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id)>;

// Post a lambda to the simulation main thread. Async CAS/state callbacks may
// fire on the io thread, so committed work is marshalled through this.
using PostCallback = std::function<void(std::function<void()>)>;

} // namespace simcore
