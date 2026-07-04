#pragma once
#include <cstdint>

constexpr uint16_t TEXTURE_FACE_DEFAULT = 0;
constexpr uint16_t TEXTURE_FACE_INPUT = 1;
constexpr uint16_t TEXTURE_FACE_OUTPUT = 2;
constexpr uint16_t TEXTURE_FACE_ENERGY = 3;
constexpr uint16_t TEXTURE_FACE_FLUID_IN = 4;
constexpr uint16_t TEXTURE_FACE_FLUID_OUT = 5;
constexpr uint16_t TEXTURE_FACE_NONE = 6;

inline uint16_t getFaceTexture(uint8_t role) {
  switch (role) {
  case 0:
    return TEXTURE_FACE_INPUT;
  case 1:
    return TEXTURE_FACE_OUTPUT;
  case 2:
    return TEXTURE_FACE_ENERGY;
  case 3:
    return TEXTURE_FACE_FLUID_IN;
  case 4:
    return TEXTURE_FACE_FLUID_OUT;
  case 5:
    return TEXTURE_FACE_DEFAULT;
  case 6:
    return TEXTURE_FACE_NONE;
  default:
    return TEXTURE_FACE_DEFAULT;
  }
}
