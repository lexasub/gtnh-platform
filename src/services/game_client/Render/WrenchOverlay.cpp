#include "Render/WrenchOverlay.h"

#include <glm/glm.hpp>

namespace wrench_overlay {

// Corner indices reference the 8 cube corners
// {0:000,1:100,2:010,3:110,4:001,5:101,6:011,7:111}.
// Face order: {0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z}.
const int kFaceCorners[6][4] = {
    {1, 3, 7, 5}, // +X
    {0, 2, 6, 4}, // -X
    {2, 3, 7, 6}, // +Y
    {0, 1, 5, 4}, // -Y
    {4, 5, 7, 6}, // +Z
    {0, 1, 3, 2}  // -Z
};
const glm::vec3 kFaceNormal[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
};

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
    for (int s = 0; s < 6; ++s) {
        if (s == faced || s == (faced ^ 1)) continue;
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
        glm::vec2 dir = e1 - e0;
        const float len = glm::length(dir);
        if (len < 1.0f) continue;
        dir /= len;
        const glm::vec2 perp(-dir.y, dir.x);
        const float half = len * 0.10f;
        const float ht = 14.0f;
        const glm::vec2 rel = m - mid;
        if (std::abs(glm::dot(rel, dir)) <= half &&
            std::abs(glm::dot(rel, perp)) <= ht) {
            return s;
        }
    }
    return -1;
}
}  // namespace wrench_overlay
