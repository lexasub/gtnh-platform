#pragma once
#include <vector>
#include <cstdint>
#include <flatbuffers/table.h>

namespace simcore {

    class IActionHandler {
    public:
        virtual ~IActionHandler() = default;
        virtual void handle(const void *table) = 0;
    };

} // namespace simcore