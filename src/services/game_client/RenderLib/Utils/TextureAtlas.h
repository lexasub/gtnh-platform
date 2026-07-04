#pragma once

#include <bgfx/bgfx.h>
#include <cstdint>
#include <string>
#include <vector>

namespace renderlib {

struct UVRect {
  float u0, v0, u1, v1;
};

class TextureAtlas {
public:
  static void Init(int tileSize = 16);
  static void Shutdown();
  static bgfx::TextureHandle GetTextureHandle();
  static bool IsTransparent(uint16_t blockId);

  static constexpr int kDefaultTilesX = 16;
  static constexpr int kDefaultTilesY = 16;

  static UVRect GetUV(uint16_t tileId, int face);

private:
  static UVRect s_uvCache[256][6];
  TextureAtlas() = delete;
};

} // namespace renderlib