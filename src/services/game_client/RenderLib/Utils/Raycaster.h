#pragma once

#include "Common/Types.h"

class IBlockQuery;

namespace renderlib {

class Raycaster {
public:
  explicit Raycaster(const IBlockQuery *blockQuery);
  static constexpr float REACH_DIST = 5.0f;

  // Find first non-air block along ray (for breaking)
  // outFaceX/Y/Z: optionally receives the face normal pointing from hit toward
  // the adjacent block on the side the ray entered (for placement).
  BlockPos GetTargetedBlock(const Ray &ray, float maxDist = REACH_DIST,
                            int *outFaceX = nullptr, int *outFaceY = nullptr,
                            int *outFaceZ = nullptr) const;

  // Find empty block adjacent to hit (for placement)
  BlockPos GetPlacementPos(const Ray &ray) const;

  // Ray-cast to the first non-air block, returning the hit block, the face it
  // entered (sideHit), and the LOCAL hit coordinates in [0,1] on that face.
  // Returns max-coord BlockPos and (0,0,0,0) if nothing hit.
  struct HitInfo {
    BlockPos pos;
    int faceX = 0, faceY = 0, faceZ = 0;   // sideHit normal (0 if none)
    float u = 0.0f, v = 0.0f;              // local UV on the entered face
  };
  HitInfo RaycastHit(const Ray &ray, float maxDist = REACH_DIST) const;

private:
  const IBlockQuery *blockQuery_;
};

} // namespace renderlib