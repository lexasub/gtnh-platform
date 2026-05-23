#pragma once
#include <cstdint>

namespace simcore {

struct PurityComponent {
    float purity;

    PurityComponent() : purity(1.0f) {}
    explicit PurityComponent(float purity) : purity(purity) {}
};

} // namespace simcore
