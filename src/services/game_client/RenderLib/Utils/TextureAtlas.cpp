#include "TextureAtlas.h"
#include <bgfx/bgfx.h>
#include <vector>
#include <cstring>

namespace renderlib {

UVRect TextureAtlas::s_uvCache[256][6] = {};

static bgfx::TextureHandle s_texture = BGFX_INVALID_HANDLE;
static int s_tileSize = 16;
static int s_atlasW = 256;
static int s_atlasH = 256;
static uint8_t s_tileX[256][6]{};
static uint8_t s_tileY[256][6]{};
static bool s_initialized = false;

static void SetTile(uint16_t blockId, uint8_t tx, uint8_t ty) {
    for (int f = 0; f < 6; ++f) {
        s_tileX[blockId][f] = tx;
        s_tileY[blockId][f] = ty;
    }
}

static void FillTileFlat(std::vector<uint8_t>& pixels, int tx, int ty, uint32_t color) {
    const int originX = tx * s_tileSize;
    const int originY = ty * s_tileSize;
    const uint8_t r = static_cast<uint8_t>((color >> 0) & 0xFF);
    const uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>((color >> 16) & 0xFF);
    const uint8_t a = static_cast<uint8_t>((color >> 24) & 0xFF);
    for (int py = 0; py < s_tileSize; ++py) {
        for (int px = 0; px < s_tileSize; ++px) {
            const int idx = ((originY + py) * s_atlasW + (originX + px)) * 4;
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
            pixels[idx + 3] = a;
        }
    }
}

static void FillTileChecker(std::vector<uint8_t>& pixels, int tx, int ty, int squares,
                            uint32_t colorA, uint32_t colorB) {
    const int originX = tx * s_tileSize;
    const int originY = ty * s_tileSize;
    const int squareSize = s_tileSize / squares;
    const uint8_t aR = (colorA >> 0) & 0xFF, aG = (colorA >> 8) & 0xFF, aB = (colorA >> 16) & 0xFF, aA = (colorA >> 24) & 0xFF;
    const uint8_t bR = (colorB >> 0) & 0xFF, bG = (colorB >> 8) & 0xFF, bB = (colorB >> 16) & 0xFF, bA = (colorB >> 24) & 0xFF;
    for (int py = 0; py < s_tileSize; ++py) {
        for (int px = 0; px < s_tileSize; ++px) {
            const int sx = px / squareSize;
            const int sy = py / squareSize;
            const bool isColorA = ((sx + sy) & 1) == 0;
            const int idx = ((originY + py) * s_atlasW + (originX + px)) * 4;
            if (isColorA) {
                pixels[idx+0] = aR; pixels[idx+1] = aG; pixels[idx+2] = aB; pixels[idx+3] = aA;
            } else {
                pixels[idx+0] = bR; pixels[idx+1] = bG; pixels[idx+2] = bB; pixels[idx+3] = bA;
            }
        }
    }
}

void TextureAtlas::Init(int tileSize) {
    if (s_initialized) return;
    s_tileSize = tileSize;
    s_atlasW = kDefaultTilesX * s_tileSize;
    s_atlasH = kDefaultTilesY * s_tileSize;

    for (uint16_t id = 1; id < 256; ++id) SetTile(id, 15, 15);
    SetTile(1, 0, 0);
    SetTile(2, 0, 1);
    s_tileX[3][2] = 0; s_tileY[3][2] = 2;
    s_tileX[3][3] = 0; s_tileY[3][3] = 1;
    for (int f : {0,1,4,5}) { s_tileX[3][f] = 0; s_tileY[3][f] = 3; }

    const float halfU = 0.5f / s_atlasW;
    const float halfV = 0.5f / s_atlasH;
    for (uint16_t id = 0; id < 256; ++id) {
        for (int f = 0; f < 6; ++f) {
            float tx = s_tileX[id][f];
            float ty = s_tileY[id][f];
            float u0 = (tx * s_tileSize) / s_atlasW + halfU;
            float v0 = (ty * s_tileSize) / s_atlasH + halfV;
            float u1 = ((tx+1)*s_tileSize) / s_atlasW - halfU;
            float v1 = ((ty+1)*s_tileSize) / s_atlasH - halfV;
            s_uvCache[id][f] = {u0, v0, u1, v1};
        }
    }

    std::vector<uint8_t> pixels(s_atlasW * s_atlasH * 4, 0);
    constexpr uint32_t kStone = 0xFF808080, kDirt = 0xFF2B5A8B, kGrassTop = 0xFF50AF4C;
    constexpr uint32_t kGrassSide = 0xFF238E6B, kDarkGray = 0xFF202020, kMagenta = 0xFFFF00FF, kBlack = 0xFF000000;
    FillTileFlat(pixels, 0, 0, kStone);
    FillTileFlat(pixels, 0, 1, kDirt);
    FillTileFlat(pixels, 0, 2, kGrassTop);
    FillTileFlat(pixels, 0, 3, kGrassSide);
    FillTileChecker(pixels, 15, 15, 8, kMagenta, kBlack);
    for (int ty = 0; ty < kDefaultTilesY; ++ty)
        for (int tx = 0; tx < kDefaultTilesX; ++tx)
            if (!((tx==0 && ty<4) || (tx==15 && ty==15)))
                FillTileFlat(pixels, tx, ty, kDarkGray);

    const bgfx::Memory* mem = bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size()));
    s_texture = bgfx::createTexture2D(static_cast<uint16_t>(s_atlasW), static_cast<uint16_t>(s_atlasH),
                                      false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, mem);
    s_initialized = true;
}

void TextureAtlas::Shutdown() {
    if (bgfx::isValid(s_texture)) bgfx::destroy(s_texture);
    s_texture = BGFX_INVALID_HANDLE;
    s_initialized = false;
}

bgfx::TextureHandle TextureAtlas::GetTextureHandle() {
    return s_texture;
}

} // namespace renderlib