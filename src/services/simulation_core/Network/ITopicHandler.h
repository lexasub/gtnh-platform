#pragma once
#include <cstdint>
#include <vector>

namespace simcore {

struct ITopicHandler {
    virtual ~ITopicHandler() = default;
    virtual void handle(const std::vector<uint8_t>& data) = 0;
};

} // namespace simcore
