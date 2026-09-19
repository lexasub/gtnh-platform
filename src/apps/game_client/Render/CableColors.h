#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <unordered_map>

inline const std::unordered_map<uint8_t, glm::vec3> CABLE_COLORS = {
    {1, {0.72f, 0.45f, 0.20f}}, // LV: copper/brown
    {2, {0.85f, 0.65f, 0.13f}}, // MV: gold/yellow
    {3, {0.40f, 0.40f, 0.40f}}, // HV: tungsten/gray
    {4, {0.60f, 0.60f, 0.80f}}, // EV: platinum/silver-blue
};
