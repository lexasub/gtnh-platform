#pragma once

#include <bgfx/bgfx.h>
#include <cstdint>
#include <string>
#include <vector>

namespace renderlib {
struct BlockFaces;

struct UVRect {
  float u0, v0, u1, v1;
};

class TextureAtlas {
public:
  static void Init(int tileSize = 16);
  static void Shutdown();
  static bgfx::TextureHandle GetTextureHandle();
  static bool IsTransparent(uint16_t blockId);
  static const BlockFaces* GetBlockFaces(uint16_t blockId);
  static UVRect GetItemUV(uint16_t itemId);

  static constexpr int kDefaultTilesX = 16;
  static constexpr int kDefaultTilesY = 16;

  static UVRect GetUV(uint16_t blockId, int face);

private:
  TextureAtlas() = delete;
};

struct BlockFaces {
  uint16_t tileX[6];
  uint16_t tileY[6];
  bool transparent;
};

} // namespace renderlib