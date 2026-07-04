#pragma once
#include <cstdint>
#include <flatbuffers/table.h>
#include <vector>

namespace simcore {

class IActionHandler {
public:
  virtual ~IActionHandler() = default;
  virtual void handle(const void *table) = 0;
};

} // namespace simcore