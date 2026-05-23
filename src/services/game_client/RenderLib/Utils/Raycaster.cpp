#include "Raycaster.h"
#include <limits>
#include <cmath>

#include "RenderLib/Common/IBlockQuery.h"

namespace renderlib {

Raycaster::Raycaster(const IBlockQuery* blockQuery) : blockQuery_(blockQuery) {}

BlockPos Raycaster::GetTargetedBlock(const Ray& ray, float maxDist,
                                      int* outFaceX, int* outFaceY, int* outFaceZ) const {
    if (maxDist <= 0.0f) {
        if (outFaceX) *outFaceX = 0;
        if (outFaceY) *outFaceY = 0;
        if (outFaceZ) *outFaceZ = 0;
        return {std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()};
    }
    float px = ray.origin.x, py = ray.origin.y, pz = ray.origin.z;
    float dx = ray.direction.x, dy = ray.direction.y, dz = ray.direction.z;
    if (std::abs(dx) < 1e-6f && std::abs(dy) < 1e-6f && std::abs(dz) < 1e-6f) {
        if (outFaceX) *outFaceX = 0;
        if (outFaceY) *outFaceY = 0;
        if (outFaceZ) *outFaceZ = 0;
        return {std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()};
    }

    int stepX = (dx > 0) ? 1 : (dx < 0) ? -1 : 0;
    int stepY = (dy > 0) ? 1 : (dy < 0) ? -1 : 0;
    int stepZ = (dz > 0) ? 1 : (dz < 0) ? -1 : 0;

    float tDeltaX = (stepX != 0) ? std::abs(1.0f / dx) : std::numeric_limits<float>::infinity();
    float tDeltaY = (stepY != 0) ? std::abs(1.0f / dy) : std::numeric_limits<float>::infinity();
    float tDeltaZ = (stepZ != 0) ? std::abs(1.0f / dz) : std::numeric_limits<float>::infinity();

    float tMaxX, tMaxY, tMaxZ;
    if (stepX > 0) tMaxX = (std::floor(px) + 1.0f - px) / dx;
    else if (stepX < 0) tMaxX = (px - std::floor(px)) / (-dx);
    else tMaxX = std::numeric_limits<float>::infinity();

    if (stepY > 0) tMaxY = (std::floor(py) + 1.0f - py) / dy;
    else if (stepY < 0) tMaxY = (py - std::floor(py)) / (-dy);
    else tMaxY = std::numeric_limits<float>::infinity();

    if (stepZ > 0) tMaxZ = (std::floor(pz) + 1.0f - pz) / dz;
    else if (stepZ < 0) tMaxZ = (pz - std::floor(pz)) / (-dz);
    else tMaxZ = std::numeric_limits<float>::infinity();

    int vx = static_cast<int>(std::floor(px));
    int vy = static_cast<int>(std::floor(py));
    int vz = static_cast<int>(std::floor(pz));

    // Track the last DDA step direction (which face was crossed)
    int lastStepX = 0, lastStepY = 0, lastStepZ = 0;

    float maxDistSq = maxDist * maxDist;

    while (true) {
        float distSq = (px-ray.origin.x)*(px-ray.origin.x) + (py-ray.origin.y)*(py-ray.origin.y) + (pz-ray.origin.z)*(pz-ray.origin.z);
        if (distSq > maxDistSq) break;
        if (blockQuery_->GetBlockAt({vx, vy, vz}) != 0) [[unlikely]] {
            // The DDA just stepped into this block from the previous cell.
            // The face normal pointing from the hit toward the adjacent block
            // is opposite to the last step direction.
            if (outFaceX) *outFaceX = -lastStepX;
            if (outFaceY) *outFaceY = -lastStepY;
            if (outFaceZ) *outFaceZ = -lastStepZ;
            return {vx, vy, vz};
        }
        //TODO, may be compute axis, tMin, zSmaller, ySmaller and switch
        if (tMaxX < tMaxY) {
            if (tMaxX < tMaxZ) {
                lastStepX = stepX; lastStepY = 0; lastStepZ = 0;
                vx += stepX;
                px = ray.origin.x + tMaxX * dx;
                py = ray.origin.y + tMaxX * dy;
                pz = ray.origin.z + tMaxX * dz;
                tMaxX += tDeltaX;
            } else {
                lastStepX = 0; lastStepY = 0; lastStepZ = stepZ;
                vz += stepZ;
                px = ray.origin.x + tMaxZ * dx;
                py = ray.origin.y + tMaxZ * dy;
                pz = ray.origin.z + tMaxZ * dz;
                tMaxZ += tDeltaZ;
            }
        } else {
            if (tMaxY < tMaxZ) {
                lastStepX = 0; lastStepY = stepY; lastStepZ = 0;
                vy += stepY;
                px = ray.origin.x + tMaxY * dx;
                py = ray.origin.y + tMaxY * dy;
                pz = ray.origin.z + tMaxY * dz;
                tMaxY += tDeltaY;
            } else {
                lastStepX = 0; lastStepY = 0; lastStepZ = stepZ;
                vz += stepZ;
                px = ray.origin.x + tMaxZ * dx;
                py = ray.origin.y + tMaxZ * dy;
                pz = ray.origin.z + tMaxZ * dz;
                tMaxZ += tDeltaZ;
            }
        }
    }
    if (outFaceX) *outFaceX = 0;
    if (outFaceY) *outFaceY = 0;
    if (outFaceZ) *outFaceZ = 0;
    return {std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()};
}

BlockPos Raycaster::GetPlacementPos(const Ray& ray) const {
    int faceNormalX = 0, faceNormalY = 0, faceNormalZ = 0;
    BlockPos hit = GetTargetedBlock(ray, REACH_DIST, &faceNormalX, &faceNormalY, &faceNormalZ);
    if (hit.x == std::numeric_limits<int32_t>::max())
        return hit;

    // faceNormal comes from the DDA: points from hit toward the adjacent block
    // on the side the ray entered. Zero normal = ray started inside the block.
    if (faceNormalX != 0 || faceNormalY != 0 || faceNormalZ != 0) {
        BlockPos placement{hit.x + faceNormalX,
                           hit.y + faceNormalY,
                           hit.z + faceNormalZ};
        if (blockQuery_->GetBlockAt(placement) == 0)
            return placement;
    }

    // Fallback: place in the direction opposite to the ray
    glm::vec3 dir = glm::normalize(ray.direction);
    return {hit.x - static_cast<int>(std::round(dir.x)),
            hit.y - static_cast<int>(std::round(dir.y)),
            hit.z - static_cast<int>(std::round(dir.z))};
}

} // namespace renderlib