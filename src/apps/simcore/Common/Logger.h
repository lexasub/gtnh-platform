#pragma once
#include <spdlog/spdlog.h>

namespace simcore {

inline void initLogger() {
  spdlog::set_level(spdlog::level::debug);
  spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
}

// Можно добавить макросы или inline функции, но spdlog и так удобен.
// Оставляем просто для единообразия.

} // namespace simcore