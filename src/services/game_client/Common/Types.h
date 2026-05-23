#pragma once

#include <common/coords/Coords.h>
#include <cstdint>
#include <glm/glm.hpp>

// Ray for picking / collision
struct Ray {
    glm::vec3 origin{};
    glm::vec3 direction{};
};

// Plane for frustum culling
struct Plane {
    glm::vec3 normal{};
    float distance = 0.0f;
    float DistanceTo(const glm::vec3& point) const { return glm::dot(normal, point) + distance; }
};

// Frustum (6 planes)
struct Frustum {
    Plane planes[6]; // left, right, bottom, top, near, far

    bool IntersectsAABB(const glm::vec3& min, const glm::vec3& max) const {
        for (int i = 0; i < 6; ++i) {
            glm::vec3 p = min;
            if (planes[i].normal.x >= 0) p.x = max.x;
            if (planes[i].normal.y >= 0) p.y = max.y;
            if (planes[i].normal.z >= 0) p.z = max.z;
            if (planes[i].DistanceTo(p) < 0) return false;
        }
        return true;
    }
};
