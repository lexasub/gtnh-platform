#pragma once
#include <cstdint>
#include <string>
#include <entt/entt.hpp>

namespace simcore {

struct WrenchCycleResult {
    bool success;
    std::string error;
    uint8_t newRole;
    uint8_t allRoles[6];
};

class WrenchHandler {
public:
    explicit WrenchHandler(entt::registry& registry) : m_registry(registry) {}
    WrenchCycleResult cycleFace(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face);
private:
    entt::registry& m_registry;
};

} // namespace simcore
