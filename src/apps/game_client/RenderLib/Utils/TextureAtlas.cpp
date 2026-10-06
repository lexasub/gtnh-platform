#include "TextureAtlas.h"
#include <engine/registry/OpenHashMap.h>
#include <engine/registry/ItemId.h>
#include <bgfx/bgfx.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <array>
#include <lodepng.h>
#include <spdlog/spdlog.h>

namespace renderlib {

// The registries contain more than 128 block/item rows. Keep enough capacity
// for the generated CSVs; OpenHashMap deliberately has no resize path.
static OpenHashMap<uint16_t, BlockFaces, 512, 0xFFFF> s_blockFaces;
static OpenHashMap<uint16_t, uint16_t, 512, 0xFFFF> s_itemIcons; // item_id → tile_id (for item icon resolution, populated from item_icons.csv)
static OpenHashMap<uint16_t, TransportMaterial, 64, 0xFFFF> s_transportMaterials;

static bgfx::TextureHandle s_texture = BGFX_INVALID_HANDLE;
static int s_tileSize = 16;
static int s_atlasW = TextureAtlas::kDefaultTilesX * 16;
static int s_atlasH = TextureAtlas::kDefaultTilesY * 16;
static bool s_initialized = false;
static uint64_t s_generation = 0;
static uint16_t s_nextAtlasSlot = 0; // tracks next free atlas slot across LoadTextureRegistry + LoadMergeRegistry

// Tile ID to source/atlas data. Atlas grid coordinates remain byte-sized (0..31).
static std::array<uint8_t, TextureAtlas::kTileCapacity> s_tileAtlasX{};
static std::array<uint8_t, TextureAtlas::kTileCapacity> s_tileAtlasY{};
static std::array<uint8_t, TextureAtlas::kTileCapacity> s_srcPosX{};
static std::array<uint8_t, TextureAtlas::kTileCapacity> s_srcPosY{};
static std::array<uint8_t, TextureAtlas::kTileCapacity> s_rotate{};
static std::array<bool, TextureAtlas::kTileCapacity> s_selfMergeOverlay{};
static std::array<bool, TextureAtlas::kTileCapacity> s_tileSourceRegistered{};
static std::array<bool, TextureAtlas::kTileCapacity> s_tileAtlasAssigned{};

static std::vector<std::string> SplitCSV(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0, end;
    while ((end = line.find(',', start)) != std::string::npos) {
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    fields.push_back(line.substr(start));
    return fields;
}

static bool ParseTileId(const std::string& value, uint16_t& tileId) {
    const int parsed = std::stoi(value);
    if (parsed < 0 || parsed >= TextureAtlas::kTileCapacity) return false;
    tileId = static_cast<uint16_t>(parsed);
    return true;
}

static bool AssignAtlasSlot(uint16_t tileId) {
    if (tileId >= TextureAtlas::kTileCapacity || s_nextAtlasSlot >= TextureAtlas::kTileCapacity) {
        spdlog::error("TextureAtlas capacity exhausted while assigning tile {}", tileId);
        return false;
    }

    const uint16_t slot = s_nextAtlasSlot++;
    s_tileAtlasX[tileId] = static_cast<uint8_t>(slot % TextureAtlas::kDefaultTilesX);
    s_tileAtlasY[tileId] = static_cast<uint8_t>(slot / TextureAtlas::kDefaultTilesX);
    s_tileAtlasAssigned[tileId] = true;
    return true;
}

static void LoadSelfMergeOverlayIds(const char* dataDir) {
    s_selfMergeOverlay.fill(false);
    const std::string filename = std::string(dataDir) + "/textures/textures_merge.csv";
    std::FILE* file = std::fopen(filename.c_str(), "r");
    if (!file) return;

    char line[1024];
    bool hasHeader = false;
    while (std::fgets(line, sizeof(line), file)) {
        std::string lineStr(line);
        if (!hasHeader) {
            hasHeader = true;
            continue;
        }
        const auto fields = SplitCSV(lineStr);
        if (fields.size() < 3) continue;
        try {
            uint16_t compositeId, overlayTileId;
            if (ParseTileId(fields[0], compositeId) &&
                ParseTileId(fields[2], overlayTileId) && compositeId == overlayTileId) {
                s_selfMergeOverlay[overlayTileId] = true;
            }
        } catch (const std::exception&) {
            // LoadMergeRegistry reports malformed rows with their full context.
        }
    }
    std::fclose(file);
}

static UVRect TileUV(uint16_t tileId) {
    const float tx = static_cast<float>(s_tileAtlasX[tileId]);
    const float ty = static_cast<float>(s_tileAtlasY[tileId]);
    const float tileSize = static_cast<float>(s_tileSize);
    const float atlasW = static_cast<float>(s_atlasW);
    const float atlasH = static_cast<float>(s_atlasH);
    const float halfU = 0.5f / atlasW;
    const float halfV = 0.5f / atlasH;
    return {
        (tx * tileSize) / atlasW + halfU,
        (ty * tileSize) / atlasH + halfV,
        ((tx + 1.0f) * tileSize) / atlasW - halfU,
        ((ty + 1.0f) * tileSize) / atlasH - halfV
    };
}

static bool LoadTransportMaterialRegistry(const char* dataDir) {
    const std::string filename = std::string(dataDir) + "/textures/transport_faces.csv";
    std::FILE* file = std::fopen(filename.c_str(), "r");
    if (!file) {
        spdlog::info("No transport face registry found (optional): {}", filename);
        return false;
    }

    char line[1024];
    bool hasHeader = false;
    while (std::fgets(line, sizeof(line), file)) {
        std::string lineStr(line);
        if (!hasHeader) {
            hasHeader = true;
            continue;
        }
        auto fields = SplitCSV(lineStr);
        if (fields.size() < 8) {
            spdlog::warn("Malformed transport face registry row: {}", lineStr);
            continue;
        }
        try {
            uint16_t blockId = ItemId::pack(fields[0]);
            TransportMaterial material{};
            bool validTiles = true;
            for (int face = 0; face < 6; ++face) {
                uint16_t tileId;
                if (!ParseTileId(fields[face + 1], tileId)) {
                    spdlog::warn("Out-of-range transport tile ID in row: {}", lineStr);
                    validTiles = false;
                    break;
                }
                material.uv[face] = TileUV(tileId);
            }
            if (!validTiles) continue;
            material.transparent = std::stoi(fields[7]) == 1;
            if (!s_transportMaterials.insert(blockId, material)) {
                spdlog::error("TextureAtlas transport registry is full; block {} ignored", fields[0]);
            }
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing transport face registry row '{}': {}", lineStr, e.what());
        }
    }
    std::fclose(file);
    return true;
}

static bool LoadTextureRegistry(const char* dataDir) {
    const std::string filename = std::string(dataDir) + "/textures/textures.csv";
    std::FILE* file = std::fopen(filename.c_str(), "r");
    if (!file) {
        spdlog::warn("Failed to open texture registry: {}", filename);
        return false;
    }
    
    char line[1024];
    bool hasHeader = false;
    while (std::fgets(line, sizeof(line), file)) {
        std::string lineStr(line);
        if (!hasHeader) {
            hasHeader = true;
            continue;
        }
        
        auto fields = SplitCSV(lineStr);
        if (fields.size() < 4) {
            spdlog::warn("Malformed texture registry row: {}", lineStr);
            continue;
        }
        
        try {
            uint16_t tileId;
            if (!ParseTileId(fields[0], tileId)) {
                spdlog::warn("Out-of-range tile ID in texture registry row: {}", lineStr);
                continue;
            }
            const int tileX = std::stoi(fields[2]);
            const int tileY = std::stoi(fields[3]);
            const int rotate = fields.size() >= 5 ? std::stoi(fields[4]) : 0;
            if (tileX < 0 || tileX > 255 || tileY < 0 || tileY > 255) {
                spdlog::warn("Out-of-range source cell in texture registry row: {}", lineStr);
                continue;
            }

            s_srcPosX[tileId] = static_cast<uint8_t>(tileX);
            s_srcPosY[tileId] = static_cast<uint8_t>(tileY);
            s_rotate[tileId] = static_cast<uint8_t>(rotate) & 3;
            s_tileSourceRegistered[tileId] = true;
            if (!s_selfMergeOverlay[tileId] && !AssignAtlasSlot(tileId)) continue;
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing texture registry row '{}': {}", lineStr, e.what());
            continue;
        }
    }
    
    std::fclose(file);
    return true;
}

static bool LoadBlockFaceRegistry(const char* dataDir) {
    const std::string filename = std::string(dataDir) + "/textures/block_faces.csv";
    std::FILE* file = std::fopen(filename.c_str(), "r");
    if (!file) {
        spdlog::warn("Failed to open block face registry: {}", filename);
        return false;
    }
    
    char line[1024];
    bool hasHeader = false;
    
    while (std::fgets(line, sizeof(line), file)) {
        std::string lineStr(line);
        if (!hasHeader) {
            hasHeader = true;
            continue;
        }
        
        auto fields = SplitCSV(lineStr);
        if (fields.size() < 8) {
            spdlog::warn("Malformed block face registry row: {}", lineStr);
            continue;
        }
        
        try {
            std::string blockIdStr = fields[0];
            if (!blockIdStr.empty() && blockIdStr.front() == '\"')
                blockIdStr = blockIdStr.substr(1);
            if (!blockIdStr.empty() && blockIdStr.back() == '\"')
                blockIdStr.pop_back();
            
            uint16_t blockId = ItemId::pack(blockIdStr);
            bool transparent = (std::stoi(fields[7]) == 1);
            
            BlockFaces faces;
            faces.transparent = transparent;
            
            static constexpr int kFaceFields[6] = {1, 2, 3, 4, 5, 6};
            bool validTiles = true;
            for (int f = 0; f < 6; ++f) {
                uint16_t tileId;
                if (!ParseTileId(fields[kFaceFields[f]], tileId)) {
                    spdlog::warn("Out-of-range tile ID in block-face row: {}", lineStr);
                    validTiles = false;
                    break;
                }
                faces.tileX[f] = s_tileAtlasX[tileId];
                faces.tileY[f] = s_tileAtlasY[tileId];
            }
            if (!validTiles) continue;

            if (!s_blockFaces.insert(blockId, faces)) {
                spdlog::error("TextureAtlas block-face registry is full; block {} will use fallback UV", blockIdStr);
            }
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing block face registry row '{}': {}", lineStr, e.what());
            continue;
        }
    }
    
    std::fclose(file);
    return true;
}

static bool LoadItemIconRegistry(const char* dataDir) {
    const std::string filename = std::string(dataDir) + "/textures/item_icons.csv";
    std::FILE* file = std::fopen(filename.c_str(), "r");
    if (!file) {
        spdlog::warn("Failed to open item icon registry: {}", filename);
        return false;
    }
    
    char line[1024];
    bool hasHeader = false;
    
    while (std::fgets(line, sizeof(line), file)) {
        std::string lineStr(line);
        if (!hasHeader) {
            hasHeader = true;
            continue;
        }
        
        auto fields = SplitCSV(lineStr);
        if (fields.size() < 2) {
            spdlog::warn("Malformed item icon registry row: {}", lineStr);
            continue;
        }
        
        try {
            std::string itemIdStr = fields[0];
            uint16_t itemId = ItemId::pack(itemIdStr);
            uint16_t tileId;
            if (!ParseTileId(fields[1], tileId)) {
                spdlog::warn("Out-of-range tile ID in item icon row: {}", lineStr);
                continue;
            }

            s_itemIcons.insert(itemId, tileId);
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing item icon registry row '{}': {}", lineStr, e.what());
            continue;
        }
    }
    
    std::fclose(file);
    return true;
}

static std::string s_dataDir = "data";

static bool LoadPNGRaw(const std::string& filename, std::vector<uint8_t>& out, unsigned int& w, unsigned int& h) {
    std::string fullPath = s_dataDir + "/textures/" + filename;
    unsigned int error = lodepng::decode(out, w, h, fullPath);
    if (error) {
        spdlog::error("Failed to load PNG '{}': {}", filename, lodepng_error_text(error));
        return false;
    }
    return true;
}

static bool LoadMergeRegistry(const char* dataDir, std::vector<uint8_t>& atlasPixels) {
    // Merge rows must be processed before face registries are resolved; their
    // composite IDs receive atlas slots during this pass.
    const std::string filename = std::string(dataDir) + "/textures/textures_merge.csv";
    std::FILE* file = std::fopen(filename.c_str(), "r");
    if (!file) {
        spdlog::info("No texture merge registry found (optional): {}", filename);
        return false;
    }

    char line[1024];
    bool hasHeader = false;

    while (std::fgets(line, sizeof(line), file)) {
        std::string lineStr(line);
        if (!hasHeader) {
            hasHeader = true;
            continue;
        }

        auto fields = SplitCSV(lineStr);
        if (fields.size() < 3) {
            spdlog::warn("Malformed merge registry row: {}", lineStr);
            continue;
        }

        try {
            uint16_t compositeId, baseTileId, overlayTileId;
            if (!ParseTileId(fields[0], compositeId) ||
                !ParseTileId(fields[1], baseTileId) ||
                !ParseTileId(fields[2], overlayTileId)) {
                spdlog::warn("Out-of-range tile ID in merge registry row: {}", lineStr);
                continue;
            }

            int tSz = s_tileSize;

            // Read overlay tile source PNG
            std::vector<uint8_t> overlayPng;
            unsigned int ovW, ovH;
            {
                // Find overlay tile's source filename from textures.csv data
                // Re-read textures.csv to get overlay's filename
                const std::string texFile = std::string(dataDir) + "/textures/textures.csv";
                std::FILE* tf = std::fopen(texFile.c_str(), "r");
                if (!tf) { spdlog::warn("Cannot re-read textures.csv for merge"); continue; }
                char tl[1024]; bool hdr = false; bool found = false;
                while (std::fgets(tl, sizeof(tl), tf)) {
                    std::string ts(tl);
                    if (!hdr) { hdr = true; continue; }
                    auto f = SplitCSV(ts);
                    uint16_t sourceTileId;
                    if (f.size() >= 4 && ParseTileId(f[0], sourceTileId) && sourceTileId == overlayTileId) {
                        if (!LoadPNGRaw(f[1], overlayPng, ovW, ovH)) { std::fclose(tf); continue; }
                        found = true; break;
                    }
                }
                std::fclose(tf);
                if (!found || overlayPng.empty()) { spdlog::warn("Overlay tile {} not found", overlayTileId); continue; }
            }

            if (!s_tileSourceRegistered[overlayTileId] || !s_tileAtlasAssigned[baseTileId]) {
                spdlog::warn("Merge row references an unloaded base/overlay tile: {}", lineStr);
                continue;
            }

            const uint8_t ovTx = s_srcPosX[overlayTileId];
            const uint8_t ovTy = s_srcPosY[overlayTileId];
            if (ovTx >= ovW / tSz || ovTy >= ovH / tSz) {
                spdlog::warn("Merge overlay source cell is out of bounds: {}", lineStr);
                continue;
            }
            const uint8_t baseTx = s_tileAtlasX[baseTileId];
            const uint8_t baseTy = s_tileAtlasY[baseTileId];

            if (!AssignAtlasSlot(compositeId)) continue;
            const uint8_t atlasTx = s_tileAtlasX[compositeId];
            const uint8_t atlasTy = s_tileAtlasY[compositeId];

            for (int py = 0; py < tSz; ++py) {
                for (int px = 0; px < tSz; ++px) {
                    // Base pixel from atlas (already loaded)
                    int bx = (baseTy * tSz + py) * s_atlasW + (baseTx * tSz + px);
                    uint8_t br = atlasPixels[bx * 4 + 0];
                    uint8_t bg = atlasPixels[bx * 4 + 1];
                    uint8_t bb = atlasPixels[bx * 4 + 2];
                    uint8_t ba = atlasPixels[bx * 4 + 3];

                    // Overlay pixel from source PNG
                    int ox = (ovTy * tSz + py) * ovW + (ovTx * tSz + px);
                    uint8_t or2 = overlayPng[ox * 4 + 0];
                    uint8_t og = overlayPng[ox * 4 + 1];
                    uint8_t ob = overlayPng[ox * 4 + 2];
                    uint8_t oa = overlayPng[ox * 4 + 3];

                    // Alpha compositing: source-over in straight-alpha RGBA.
                    // Blend premultiplied contributions, then un-premultiply so
                    // the atlas shader can apply the resulting alpha exactly once.
                    float srcA = oa / 255.0f;
                    float dstA = ba / 255.0f;
                    float outA = srcA + dstA * (1.0f - srcA);
                    float outR = or2 * srcA + br * dstA * (1.0f - srcA);
                    float outG = og * srcA + bg * dstA * (1.0f - srcA);
                    float outB = ob * srcA + bb * dstA * (1.0f - srcA);
                    uint8_t cr = outA > 0.0f ? static_cast<uint8_t>(outR / outA) : 0;
                    uint8_t cg = outA > 0.0f ? static_cast<uint8_t>(outG / outA) : 0;
                    uint8_t cb = outA > 0.0f ? static_cast<uint8_t>(outB / outA) : 0;
                    uint8_t ca = static_cast<uint8_t>(outA * 255.0f);

                    int dx = (atlasTy * tSz + py) * s_atlasW + (atlasTx * tSz + px);
                    atlasPixels[dx * 4 + 0] = cr;
                    atlasPixels[dx * 4 + 1] = cg;
                    atlasPixels[dx * 4 + 2] = cb;
                    atlasPixels[dx * 4 + 3] = ca;
                }
            }

            spdlog::info("Merged tile {} = base {} + overlay {}", compositeId, baseTileId, overlayTileId);
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing merge row '{}': {}", lineStr, e.what());
        }
    }

    std::fclose(file);
    return true;
}

static bool LoadSourcePNG(const std::string& filename, std::vector<uint8_t>& atlasPixels,
                         int atlasW, [[maybe_unused]] int atlasH, const std::vector<uint16_t>& tileIds) {
    std::string fullPath = s_dataDir + "/textures/" + filename;
    
    std::vector<unsigned char> pngData;
    unsigned int width, height;
    
    unsigned int error = lodepng::decode(pngData, width, height, fullPath);
    if (error) {
        spdlog::error("Failed to load PNG '{}': {}", filename, lodepng_error_text(error));
        return false;
    }
    
    if (width % 16 != 0 || height % 16 != 0) {
        spdlog::warn("PNG '{}' dimensions not divisible by 16: {}x{}", filename, width, height);
    }
    
    for (uint16_t tileId : tileIds) {
        uint8_t srcTx = s_srcPosX[tileId];
        uint8_t srcTy = s_srcPosY[tileId];
        uint8_t atlasTx = s_tileAtlasX[tileId];
        uint8_t atlasTy = s_tileAtlasY[tileId];
        
        if (srcTx >= width / 16 || srcTy >= height / 16) {
            spdlog::warn("Tile {} at source ({},{}) is out of bounds for PNG {} ({}x{})", 
                        tileId, srcTx, srcTy, filename, width, height);
            continue;
        }
        
        int tileSz = s_tileSize;
        uint8_t rot = s_rotate[tileId] & 3;
        for (int py = 0; py < tileSz; ++py) {
            for (int px = 0; px < tileSz; ++px) {
                int srcX = srcTx * tileSz + px;
                int srcY = srcTy * tileSz + py;
                int srcIdx = (srcY * width + srcX) * 4;
                
                int dstX, dstY;
                switch (rot) {
                    case 1: dstX = atlasTx * tileSz + (tileSz - 1 - py); dstY = atlasTy * tileSz + px;       break; // 90°CW
                    case 2: dstX = atlasTx * tileSz + (tileSz - 1 - px); dstY = atlasTy * tileSz + (tileSz - 1 - py); break; // 180°
                    case 3: dstX = atlasTx * tileSz + py;                dstY = atlasTy * tileSz + (tileSz - 1 - px); break; // 270°CW
                    default: dstX = atlasTx * tileSz + px;               dstY = atlasTy * tileSz + py;              break; // 0°
                }
                int dstIdx = ((dstY * atlasW + dstX) * 4);
                
                for (int c = 0; c < 4; ++c) {
                    atlasPixels[dstIdx + c] = pngData[srcIdx + c];
                }
            }
        }
    }
    
    return true;
}

void TextureAtlas::Init(int tileSize) {
    if (s_initialized) return;
    s_tileSize = tileSize;
    s_atlasW = kDefaultTilesX * s_tileSize;
    s_atlasH = kDefaultTilesY * s_tileSize;

    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps || caps->limits.maxTextureSize < static_cast<uint32_t>(std::max(s_atlasW, s_atlasH))) {
        spdlog::error("TextureAtlas requires a GPU texture size of at least {} pixels", std::max(s_atlasW, s_atlasH));
        return;
    }

    for (int id = 0; id < kTileCapacity; ++id) {
        s_tileAtlasX[id] = static_cast<uint8_t>(id % kDefaultTilesX);
        s_tileAtlasY[id] = static_cast<uint8_t>(id / kDefaultTilesX);
        s_srcPosX[id] = static_cast<uint8_t>(id % kDefaultTilesX);
        s_srcPosY[id] = static_cast<uint8_t>(id / kDefaultTilesX);
        s_rotate[id] = 0;
    }
    s_tileSourceRegistered.fill(false);
    s_tileAtlasAssigned.fill(false);

#ifdef DATA_DIR
    s_dataDir = DATA_DIR;
#else
    s_dataDir = "data";
#endif
    const char* dataDir = s_dataDir.c_str();
    s_nextAtlasSlot = 0;
    LoadSelfMergeOverlayIds(dataDir);
    LoadTextureRegistry(dataDir);
    
    // Face entries store resolved atlas slots, so load them only after source
    // tiles and merge composites have been assigned their final slots.
    std::vector<uint8_t> atlasPixels(s_atlasW * s_atlasH * 4, 0);
    
    std::vector<std::pair<std::string, std::vector<uint16_t>>> fileToTileIds;
    {
        auto filename = std::string(dataDir) + "/textures/textures.csv";
        std::FILE* file = std::fopen(filename.c_str(), "r");
        if (file) {
            char line[1024];
            bool hasHeader = false;
            while (std::fgets(line, sizeof(line), file)) {
                std::string lineStr(line);
                if (!hasHeader) {
                    hasHeader = true;
                    continue;
                }
                auto fields = SplitCSV(lineStr);
                if (fields.size() >= 4) {
                    try {
                        uint16_t tileId;
                        if (!ParseTileId(fields[0], tileId) || !s_tileSourceRegistered[tileId] ||
                            !s_tileAtlasAssigned[tileId] || s_selfMergeOverlay[tileId]) continue;
                        std::string filename = fields[1];
                        auto it = std::find_if(fileToTileIds.begin(), fileToTileIds.end(),
                                               [&filename](const auto& pair) { return pair.first == filename; });
                        if (it != fileToTileIds.end()) {
                            it->second.push_back(tileId);
                        } else {
                            fileToTileIds.emplace_back(filename, std::vector<uint16_t>{tileId});
                        }
                    } catch (...) {}
                }
            }
            std::fclose(file);
        }
    }
    
    bool sourceLoadFailed = false;
    for (const auto& filePair : fileToTileIds) {
        sourceLoadFailed = !LoadSourcePNG(filePair.first, atlasPixels, s_atlasW, s_atlasH, filePair.second) || sourceLoadFailed;
    }
    if (sourceLoadFailed) {
        spdlog::error("TextureAtlas: one or more source packs failed to load from {}", s_dataDir);
    }

    LoadMergeRegistry(dataDir, atlasPixels);

    // Resolve block faces after source and composite tiles have final atlas
    // slots. Loading earlier turns every registry tile into the default slot.
    LoadBlockFaceRegistry(dataDir);
    LoadItemIconRegistry(dataDir);
    LoadTransportMaterialRegistry(dataDir);

    // Transport materials resolve tile IDs to atlas UVs after all source and
    // composite slots have been assigned.

    // Mark atlas slots already filled by loaded source tiles + merge tiles
    std::array<bool, kTileCapacity> slotUsed{};
    for (const auto& filePair : fileToTileIds) {
        for (uint16_t tileId: filePair.second) {
            const size_t slot = static_cast<size_t>(s_tileAtlasY[tileId]) * kDefaultTilesX + s_tileAtlasX[tileId];
            slotUsed[slot] = true;
        }
    }
    for (int i = 0; i < s_nextAtlasSlot; ++i) {
        slotUsed[i] = true;
    }
    
    // Fill unused atlas slots with magenta/black checkerboard
    for (int ty = 0; ty < kDefaultTilesY; ++ty) {
        for (int tx = 0; tx < kDefaultTilesX; ++tx) {
            if (slotUsed[ty * kDefaultTilesX + tx]) continue;
            int originX = tx * s_tileSize;
            int originY = ty * s_tileSize;
            for (int py = 0; py < s_tileSize; ++py) {
                for (int px = 0; px < s_tileSize; ++px) {
                    int sx = px / 8;
                    int sy = py / 8;
                    bool isMagenta = ((sx + sy) & 1) == 0;
                    int idx = ((originY + py) * s_atlasW + (originX + px)) * 4;
                    if (isMagenta) {
                        atlasPixels[idx + 0] = 0xFF; atlasPixels[idx + 1] = 0x00; atlasPixels[idx + 2] = 0xFF; atlasPixels[idx + 3] = 0xFF;
                    } else {
                        atlasPixels[idx + 0] = 0x00; atlasPixels[idx + 1] = 0x00; atlasPixels[idx + 2] = 0x00; atlasPixels[idx + 3] = 0xFF;
                    }
                }
            }
        }
    }
    
    const bgfx::Memory* mem = bgfx::copy(atlasPixels.data(), static_cast<uint32_t>(atlasPixels.size()));
    s_texture = bgfx::createTexture2D(static_cast<uint16_t>(s_atlasW), static_cast<uint16_t>(s_atlasH),
                                      false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, mem);
    
    s_initialized = true;
    ++s_generation;
}

bool TextureAtlas::IsTransparent(uint16_t blockId) {
    if (!s_initialized) return false;
    auto* entry = s_blockFaces.find(blockId);
    return entry ? entry->transparent : false;
}

UVRect TextureAtlas::GetUV(uint16_t blockId, int face) {
    // Never return the whole atlas for an unmapped block: sampling across the
    // atlas can hit transparent pixels and makes a placed block look invisible.
    // Tile 16 is the canonical opaque generic fallback used by block_faces.csv.
    if (!s_initialized || face < 0 || face >= 6) return TileUV(16);
    auto* entry = s_blockFaces.find(blockId);
    if (!entry) return TileUV(16);
    float tx = entry->tileX[face];
    float ty = entry->tileY[face];
    float halfU = 0.5f / s_atlasW;
    float halfV = 0.5f / s_atlasH;
    return {
        (tx * s_tileSize) / s_atlasW + halfU,
        (ty * s_tileSize) / s_atlasH + halfV,
        ((tx+1) * s_tileSize) / s_atlasW - halfU,
        ((ty+1) * s_tileSize) / s_atlasH - halfV
    };
}

const TransportMaterial* TextureAtlas::GetTransportMaterial(uint16_t blockId) {
    if (!s_initialized) return nullptr;
    return s_transportMaterials.find(blockId);
}

UVRect TextureAtlas::GetTileUV(uint16_t tileId) {
    if (!s_initialized) return {0, 0, 1, 1};
    if (tileId >= kTileCapacity) return TileUV(16);
    return TileUV(tileId);
}

uint64_t TextureAtlas::GetGeneration() {
    return s_generation;
}

void TextureAtlas::Shutdown() {
    s_initialized = false;
    if (bgfx::isValid(s_texture)) bgfx::destroy(s_texture);
    s_texture = BGFX_INVALID_HANDLE;
    s_blockFaces.clear();
    s_itemIcons.clear();
    s_transportMaterials.clear();
}

bgfx::TextureHandle TextureAtlas::GetTextureHandle() {
    return s_texture;
}

const BlockFaces* TextureAtlas::GetBlockFaces(uint16_t blockId) {
    if (!s_initialized) return nullptr;
    auto* entry = s_blockFaces.find(blockId);
    return entry;
}

UVRect TextureAtlas::GetItemUV(uint16_t itemId) {
    if (!s_initialized) return {0, 0, 1, 1};
    
    // Resolution chain: 1. item_icons.csv, 2. block_faces.csv (first face), 3. default UV
    if (auto* itemEntry = s_itemIcons.find(itemId)) {
        uint16_t tileId = *itemEntry;
        if (tileId >= kTileCapacity) return TileUV(16);
        float tx = s_tileAtlasX[tileId];
        float ty = s_tileAtlasY[tileId];
        float halfU = 0.5f / s_atlasW;
        float halfV = 0.5f / s_atlasH;
        return {
            (tx * s_tileSize) / s_atlasW + halfU,
            (ty * s_tileSize) / s_atlasH + halfV,
            ((tx+1) * s_tileSize) / s_atlasW - halfU,
            ((ty+1) * s_tileSize) / s_atlasH - halfV
        };
    }
    
    // Fallback to block_faces.csv (first face)
    if (auto* blockEntry = s_blockFaces.find(itemId)) {
        float tx = blockEntry->tileX[0];
        float ty = blockEntry->tileY[0];
        float halfU = 0.5f / s_atlasW;
        float halfV = 0.5f / s_atlasH;
        return {
            (tx * s_tileSize) / s_atlasW + halfU,
            (ty * s_tileSize) / s_atlasH + halfV,
            ((tx+1) * s_tileSize) / s_atlasW - halfU,
            ((ty+1) * s_tileSize) / s_atlasH - halfV
        };
    }
    
    // Default UV (magenta/black checkerboard)
    return {0, 0, 1, 1};
}

} // namespace renderlib