#pragma once
#include <cstdint>
namespace simcore {
enum class OverheatState : uint8_t { NONE = 0, WARNING = 1, CRITICAL = 2 };
struct OverheatComponent {
    OverheatState state = OverheatState::NONE;
    uint32_t ticks_at_critical = 0;
};
} // namespace simcore