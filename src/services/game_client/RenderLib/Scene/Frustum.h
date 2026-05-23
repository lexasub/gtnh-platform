#pragma once

#include <glm/glm.hpp>
#include <array>

namespace renderlib {

    struct Plane {
        glm::vec3 normal;
        float distance;
    };

    class Frustum {
    public:
        Frustum() = default;
        void BuildFromViewProj(const glm::mat4& vp);
        bool IntersectsAABB(const glm::vec3& min, const glm::vec3& max) const;

    private:
        std::array<Plane, 6> planes_;
    };

} // namespace renderlib