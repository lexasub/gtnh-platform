#pragma once
#include <glm/glm.hpp>

#include "Common/Types.h"

namespace wrench_overlay {

// Wrench overlay geometry — shared by the ImGui overlay (drawing) and
// HitTestWrenchBar (mouse hit-testing) so the clickable bars always match what
// is drawn. Corner indices reference the 8 cube corners
// {0:000,1:100,2:010,3:110,4:001,5:101,6:011,7:111}.
// Face order: {0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z}.
extern const int kFaceCorners[6][4];
extern const glm::vec3 kFaceNormal[6];

// Returns the overlay face index whose connection bar contains the mouse, or -1.
// (mouseX, mouseY) are window pixels in [0,width)x[0,height).
int HitTestWrenchBar(const glm::mat4& view, const glm::mat4& proj,
                     int width, int height, const glm::vec3& camPos,
                     const BlockPos& hb, double mouseX, double mouseY);
}  // namespace wrench_overlay
