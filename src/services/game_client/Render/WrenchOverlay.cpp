#include "Render/WrenchOverlay.h"

#include <glm/glm.hpp>

namespace wrench_overlay {

// kFaceCorners / kFaceNormal are re-exported from wrench_grid (see
// WrenchOverlay.h) — single source of truth for the cube geometry.

int HitTestWrenchBar(const glm::mat4& view, const glm::mat4& proj,
                     int width, int height, const glm::vec3& camPos,
                     const BlockPos& hb, double mouseX, double mouseY) {
    const glm::mat4 vp = proj * view;
    glm::vec3 corners[8] = {
        glm::vec3(hb.x,       hb.y,       hb.z),
        glm::vec3(hb.x + 1,   hb.y,       hb.z),
        glm::vec3(hb.x,       hb.y + 1,   hb.z),
        glm::vec3(hb.x + 1,   hb.y + 1,   hb.z),
        glm::vec3(hb.x,       hb.y,       hb.z + 1),
        glm::vec3(hb.x + 1,   hb.y,       hb.z + 1),
        glm::vec3(hb.x,       hb.y + 1,   hb.z + 1),
        glm::vec3(hb.x + 1,   hb.y + 1,   hb.z + 1),
    };
    glm::vec2 screen[8];
    bool offscreen[8] = {};
    for (int i = 0; i < 8; ++i) {
        const glm::vec4 clip = vp * glm::vec4(corners[i], 1.0f);
        if (clip.w <= 0.0f) { offscreen[i] = true; continue; }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        screen[i] = glm::vec2((ndc.x * 0.5f + 0.5f) * width,
                              (-ndc.y * 0.5f + 0.5f) * height);
    }

    const glm::vec3 blockCenter(hb.x + 0.5f, hb.y + 0.5f, hb.z + 0.5f);
    const glm::vec3 toBlock = blockCenter - camPos;
    int faced = 0;
    float best = -1e9f;
    for (int f = 0; f < 6; ++f) {
        const float d = glm::dot(kFaceNormal[f], toBlock);
        if (d > best) { best = d; faced = f; }
    }

    const glm::vec2 m(static_cast<float>(mouseX), static_cast<float>(mouseY));

    // GT-style: 4 connection BARS on the edges of the FACED face (each = the
    // SIDE face it borders) + 4 CORNER CROSSES (each = the FAR face). Bars are
    // checked FIRST (they cover most of each edge), crosses only near the
    // corner points — so a bar hit never returns the far face.
    float bestBarDist = 1e9f;
    int bestBarFace = -1;
    for (int s = 0; s < 6; ++s) {
        if (s == faced || s == (faced ^ 1)) continue;   // only the 4 side faces
        // Midpoint of the edge shared between face s and the faced face.
        int shared[2];
        int k = 0;
        for (int c = 0; c < 4 && k < 2; ++c) {
            for (int d = 0; d < 4; ++d) {
                if (kFaceCorners[s][c] == kFaceCorners[faced][d]) {
                    shared[k++] = kFaceCorners[s][c];
                    break;
                }
            }
        }
        if (k < 2) continue;
        if (offscreen[shared[0]] || offscreen[shared[1]]) continue;
        const glm::vec2 e0 = screen[shared[0]];
        const glm::vec2 e1 = screen[shared[1]];
        const glm::vec2 mid = (e0 + e1) * 0.5f;
        // Distance from cursor to this bar's midpoint (bar is ~16px tall).
        const float d = glm::distance(m, mid);
        if (d < bestBarDist) { bestBarDist = d; bestBarFace = s; }
    }
    constexpr float kBarR = 18.0f;   // hit radius around the bar midpoint
    if (bestBarFace >= 0 && bestBarDist <= kBarR) return bestBarFace;

    // Corner crosses (FAR face) — small radius right at the corner points, so
    // they only trigger when the cursor is clearly on a corner, never a bar.
    float bestCornerDist = 1e9f;
    int bestCorner = -1;
    for (int i = 0; i < 4; ++i) {
        if (offscreen[kFaceCorners[faced][i]]) continue;
        const glm::vec2 c = screen[kFaceCorners[faced][i]];
        const float d = glm::distance(m, c);
        if (d < bestCornerDist) { bestCornerDist = d; bestCorner = i; }
    }
    constexpr float kCrossR = 9.0f;   // only right at the corner
    if (bestCorner >= 0 && bestCornerDist <= kCrossR) return faced ^ 1;

    return -1;
}
}  // namespace wrench_overlay
