#pragma once

namespace simcore {

struct ISystem {
    virtual ~ISystem() = default;
    virtual void tick(float dt) = 0;
};

} // namespace simcore
