#include "validation.h"
#include "../InventoryValidator.h"
#include <spdlog/spdlog.h>

namespace gtnh {
namespace validation {

ValidationService::ValidationService() : m_initialized(false) {}

ValidationService::~ValidationService() {
    shutdown();
}

bool ValidationService::initialize() {
    spdlog::info("ValidationService initializing...");
    m_initialized = true;
    return true;
}

void ValidationService::shutdown() {
    spdlog::info("ValidationService shutting down...");
    m_initialized = false;
}

std::string ValidationService::validateItemStack(uint16_t itemId, uint8_t count, uint16_t meta) {
    return InventoryValidator::ValidateItemStack(itemId, count, meta);
}

std::string ValidationService::validateItemId(uint16_t itemId) {
    if (itemId > 65535) {
        return "item_id must be <= 65535";
    }
    return "";
}

std::string ValidationService::validateCount(uint8_t count) {
    if (count > 64) {
        return "count must be <= 64";
    }
    return "";
}

std::string ValidationService::validateMeta(uint16_t meta) {
    if (meta > 65535) {
        return "meta must be <= 65535";
    }
    return "";
}

} // namespace validation
} // namespace gtnh
