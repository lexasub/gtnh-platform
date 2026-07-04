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

private:
  const IBlockQuery *blockQuery_;
};

} // namespace renderlib