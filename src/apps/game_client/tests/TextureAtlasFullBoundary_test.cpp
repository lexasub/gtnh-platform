#include "RenderLib/Utils/TextureAtlas.h"

#include <bgfx/bgfx.h>
#include <lodepng.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef DATA_DIR
#error DATA_DIR must point at the test texture fixture
#endif

namespace {
constexpr int kCapacity = renderlib::TextureAtlas::kTileCapacity;

bool createFixture() {
    const std::filesystem::path root(DATA_DIR);
    const auto textureDir = root / "textures";
    const auto packsDir = textureDir / "packs";
    std::filesystem::create_directories(packsDir);

    std::ofstream textures(textureDir / "textures.csv");
    if (!textures) return false;
    textures << "tile_id,filename,tile_x,tile_y,rotate\n";
    for (int id = 0; id < kCapacity; ++id) {
        textures << id << ",packs/tiles.png,0,0,0\n";
    }

    std::ofstream merges(textureDir / "textures_merge.csv");
    if (!merges) return false;
    merges << "composite_id,base_tile_id,overlay_tile_id\n"
           << "1023,0,1023\n" // self-merge uses the final legal physical slot
           << "0,1,2\n";      // malformed collision attempts one slot past capacity

    std::ofstream faces(textureDir / "block_faces.csv");
    if (!faces) return false;
    faces << "block_id,face_px,face_nx,face_py,face_ny,face_pz,face_nz,transparent\n";

    std::ofstream icons(textureDir / "item_icons.csv");
    if (!icons) return false;
    icons << "item_id,tile_id\n";

    std::ofstream transport(textureDir / "transport_faces.csv");
    if (!transport) return false;
    transport << "block_id,face_px,face_nx,face_py,face_ny,face_pz,face_nz,transparent\n";

    std::vector<unsigned char> pixels(16 * 16 * 4, 0);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 80;
        pixels[i + 1] = 120;
        pixels[i + 2] = 160;
        pixels[i + 3] = 255;
    }
    return lodepng::encode((packsDir / "tiles.png").string(), pixels, 16, 16) == 0;
}

bool matchesSlot(renderlib::UVRect uv, uint16_t slot) {
    constexpr float kAtlasSize = 512.0f;
    constexpr float kTileSize = 16.0f;
    constexpr float kHalfTexel = 0.5f / kAtlasSize;
    const float expectedU = (static_cast<float>(slot % 32) * kTileSize) / kAtlasSize + kHalfTexel;
    const float expectedV = (static_cast<float>(slot / 32) * kTileSize) / kAtlasSize + kHalfTexel;
    return std::fabs(uv.u0 - expectedU) < 0.000001f && std::fabs(uv.v0 - expectedV) < 0.000001f;
}

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    renderlib::TextureAtlas::Shutdown();
    bgfx::shutdown();
    return 1;
}
} // namespace

int main() {
    if (!createFixture()) {
        std::fprintf(stderr, "could not create full-capacity atlas fixture\n");
        return 1;
    }

    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 64;
    init.resolution.height = 64;
    init.resolution.reset = BGFX_RESET_NONE;
    if (!bgfx::init(init)) {
        std::fprintf(stderr, "bgfx Noop renderer initialization failed\n");
        return 1;
    }

    renderlib::TextureAtlas::Init(16);
    if (!bgfx::isValid(renderlib::TextureAtlas::GetTextureHandle())) {
        return fail("full-capacity atlas initialization failed");
    }
    if (!matchesSlot(renderlib::TextureAtlas::GetTileUV(0), 0)) {
        return fail("overflow merge corrupted the first allocated tile");
    }
    if (!matchesSlot(renderlib::TextureAtlas::GetTileUV(1023), 1023)) {
        return fail("self-merge did not receive the final valid slot");
    }

    const auto invalidTile = renderlib::TextureAtlas::GetTileUV(1024);
    if (invalidTile.u1 - invalidTile.u0 >= 0.1f ||
        invalidTile.v1 - invalidTile.v0 >= 0.1f) {
        return fail("out-of-range tile did not resolve to a single fallback tile");
    }

    renderlib::TextureAtlas::Shutdown();
    bgfx::shutdown();
    return 0;
}
