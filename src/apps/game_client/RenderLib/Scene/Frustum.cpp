#include "Frustum.h"
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

namespace renderlib {

    void Frustum::BuildFromViewProj(const glm::mat4& vp) {
        // Left
        planes_[0].normal = glm::vec3(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0]);
        planes_[0].distance = vp[3][3] + vp[3][0];
        // Right
        planes_[1].normal = glm::vec3(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0]);
        planes_[1].distance = vp[3][3] - vp[3][0];
        // Bottom
        planes_[2].normal = glm::vec3(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1]);
        planes_[2].distance = vp[3][3] + vp[3][1];
        // Top
        planes_[3].normal = glm::vec3(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1]);
        planes_[3].distance = vp[3][3] - vp[3][1];
        // Near
        planes_[4].normal = glm::vec3(vp[0][3] + vp[0][2], vp[1][3] + vp[1][2], vp[2][3] + vp[2][2]);
        planes_[4].distance = vp[3][3] + vp[3][2];
        // Far
        planes_[5].normal = glm::vec3(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2]);
        planes_[5].distance = vp[3][3] - vp[3][2];

        for (auto& p : planes_) {
            float len = glm::length(p.normal);
            if (len > 0.0f) {
                p.normal /= len;
                p.distance /= len;
            }
        }
    }

    bool Frustum::IntersectsAABB(const glm::vec3& min, const glm::vec3& max) const {
        for (const auto& p : planes_) {
            glm::vec3 positive(
                p.normal.x > 0 ? max.x : min.x,
                p.normal.y > 0 ? max.y : min.y,
                p.normal.z > 0 ? max.z : min.z);
            if (glm::dot(p.normal, positive) + p.distance < 0)
                return false;
        }
        return true;
    }

} // namespace renderlib