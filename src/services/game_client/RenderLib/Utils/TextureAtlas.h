#pragma once

#include <bgfx/bgfx.h>
#include <cstdint>

namespace renderlib {

    struct UVRect {
        float u0, v0, u1, v1;
    };

    class TextureAtlas {
    public:
        static void Init(int tileSize = 16);
        static void Shutdown();
        static bgfx::TextureHandle GetTextureHandle();

        static constexpr int kDefaultTilesX = 16;
        static constexpr int kDefaultTilesY = 16;

        inline static UVRect GetUV(uint16_t tileId, int face) {
            // tileId не должен выходить за 255, face < 6
            return s_uvCache[tileId][face];
        }

    private:
        TextureAtlas() = delete;
        static UVRect s_uvCache[256][6];
    };

} // namespace renderlib