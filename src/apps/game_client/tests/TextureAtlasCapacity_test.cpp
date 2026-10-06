#include "RenderLib/Utils/TextureAtlas.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef DATA_DIR
#error DATA_DIR must point at the canonical texture registry
#endif

namespace {
using Merge = std::array<uint16_t, 3>;

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    return fields;
}

bool validId(int id) {
    return id >= 0 && id < renderlib::TextureAtlas::kTileCapacity;
}

bool expectedAtlasLayout(std::array<uint16_t, renderlib::TextureAtlas::kTileCapacity>& slots,
                         std::array<bool, renderlib::TextureAtlas::kTileCapacity>& assigned,
                         std::vector<Merge>& merges,
                         uint16_t& slotCount) {
    std::array<bool, renderlib::TextureAtlas::kTileCapacity> selfMerge{};
    const std::string textureRoot = std::string(DATA_DIR) + "/textures/";

    std::ifstream mergeFile(textureRoot + "textures_merge.csv");
    std::string line;
    if (!std::getline(mergeFile, line)) return false;
    while (std::getline(mergeFile, line)) {
        if (line.empty() || line.front() == '#') continue;
        const auto fields = splitCsv(line);
        if (fields.size() < 3) return false;
        const int composite = std::stoi(fields[0]);
        const int base = std::stoi(fields[1]);
        const int overlay = std::stoi(fields[2]);
        if (!validId(composite) || !validId(base) || !validId(overlay)) return false;
        merges.push_back({static_cast<uint16_t>(composite), static_cast<uint16_t>(base),
                          static_cast<uint16_t>(overlay)});
        if (composite == overlay) selfMerge[overlay] = true;
    }

    std::ifstream textureFile(textureRoot + "textures.csv");
    if (!std::getline(textureFile, line)) return false;
    slotCount = 0;
    while (std::getline(textureFile, line)) {
        if (line.empty() || line.front() == '#') continue;
        const auto fields = splitCsv(line);
        if (fields.size() < 4) return false;
        const int tileId = std::stoi(fields[0]);
        if (!validId(tileId)) return false;
        if (selfMerge[tileId]) continue;
        if (slotCount >= renderlib::TextureAtlas::kTileCapacity) return false;
        slots[tileId] = slotCount++;
        assigned[tileId] = true;
    }

    for (const auto& merge : merges) {
        const bool selfMerge = merge[0] == merge[2];
        if (!assigned[merge[1]] || (!selfMerge && !assigned[merge[2]])) return false;
        if (slotCount >= renderlib::TextureAtlas::kTileCapacity) return false;
        slots[merge[0]] = slotCount++;
        assigned[merge[0]] = true;
    }
    return true;
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
    std::array<uint16_t, renderlib::TextureAtlas::kTileCapacity> slots{};
    std::array<bool, renderlib::TextureAtlas::kTileCapacity> assigned{};
    std::vector<Merge> merges;
    uint16_t expectedSlotCount = 0;
    if (!expectedAtlasLayout(slots, assigned, merges, expectedSlotCount)) {
        std::fprintf(stderr, "could not derive a valid physical atlas layout from texture registries\n");
        return 1;
    }
    if (expectedSlotCount > renderlib::TextureAtlas::kTileCapacity) {
        std::fprintf(stderr, "texture registries exceed physical atlas capacity\n");
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
        return fail("texture atlas initialization failed");
    }

    for (const auto& merge : merges) {
        if (!matchesSlot(renderlib::TextureAtlas::GetTileUV(merge[0]), slots[merge[0]])) {
            return fail("merge composite UV does not match expected atlas allocation");
        }
    }
    if (expectedSlotCount > renderlib::TextureAtlas::kTileCapacity) {
        return fail("expected source and merge slots exceed atlas capacity");
    }

    const auto lastTile = renderlib::TextureAtlas::GetTileUV(1023);
    if (!matchesSlot(lastTile, 1023) || lastTile.u1 > 1.0f || lastTile.v1 > 1.0f) {
        return fail("tile 1023 UV is outside the 32x32 atlas");
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
