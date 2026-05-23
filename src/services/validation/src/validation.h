#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gtnh {
namespace validation {

class ValidationService {
public:
    ValidationService();
    ~ValidationService();

    bool initialize();
    void shutdown();

    // Item/block validation
    std::string validateItemStack(uint16_t itemId, uint8_t count, uint16_t meta);
    std::string validateItemId(uint16_t itemId);
    std::string validateCount(uint8_t count);
    std::string validateMeta(uint16_t meta);

private:
    bool m_initialized = false;
};

} // namespace validation
} // namespace gtnh
