#include "TextureAtlas.h"
#include "../../common/OpenHashMap.h"
#include <common/ItemId.h>
#include <bgfx/bgfx.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <lodepng.h>
#include <spdlog/spdlog.h>

namespace renderlib {

static OpenHashMap<uint16_t, BlockFaces, 128, 0xFFFF> s_blockFaces;
static OpenHashMap<uint16_t, uint16_t, 128, 0xFFFF> s_itemIcons; // item_id → tile_id (for item icon resolution, populated from item_icons.csv)

static bgfx::TextureHandle s_texture = BGFX_INVALID_HANDLE;
static int s_tileSize = 16;
static int s_atlasW = 256;
static int s_atlasH = 256;
static bool s_initialized = false;
static uint8_t s_nextAtlasSlot = 0; // tracks next free atlas slot across LoadTextureRegistry + LoadMergeRegistry

// tile_id → atlas grid position (populated from textures.csv, fallback to id%16, id/16)
static uint8_t s_tileAtlasX[256];
static uint8_t s_tileAtlasY[256];
static uint8_t s_srcPosX[256]; // tile_id → source PNG tile X (from textures.csv)
static uint8_t s_srcPosY[256]; // tile_id → source PNG tile Y (from textures.csv)
static uint8_t s_rotate[256];  // tile_id → rotation: 0=0°, 1=90°CW, 2=180°, 3=270°CW

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
            uint16_t tileId = static_cast<uint16_t>(std::stoi(fields[0]));
            uint8_t tileX = static_cast<uint8_t>(std::stoi(fields[2]));
            uint8_t tileY = static_cast<uint8_t>(std::stoi(fields[3]));
            uint8_t rotate = fields.size() >= 5 ? static_cast<uint8_t>(std::stoi(fields[4])) : 0;
            
            // Auto-assign atlas slot sequentially; tileX/Y are source PNG position only
            s_tileAtlasX[tileId] = s_nextAtlasSlot % TextureAtlas::kDefaultTilesX;
            s_tileAtlasY[tileId] = s_nextAtlasSlot / TextureAtlas::kDefaultTilesX;
            s_srcPosX[tileId] = tileX;
            s_srcPosY[tileId] = tileY;
            s_rotate[tileId] = rotate & 3;
            s_nextAtlasSlot++;
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
            for (int f = 0; f < 6; ++f) {
                uint16_t tileId = static_cast<uint16_t>(std::stoi(fields[kFaceFields[f]]));
                faces.tileX[f] = s_tileAtlasX[tileId];
                faces.tileY[f] = s_tileAtlasY[tileId];
            }
            
            s_blockFaces.insert(blockId, faces);
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
            uint16_t tileId = static_cast<uint16_t>(std::stoi(fields[1]));
            
            s_itemIcons.insert(itemId, tileId);
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing item icon registry row '{}': {}", lineStr, e.what());
            continue;
        }
    }
    
    std::fclose(file);
    return true;
}

static bool LoadPNGRaw(const std::string& filename, std::vector<uint8_t>& out, unsigned int& w, unsigned int& h) {
    std::string fullPath = std::string("data/textures/") + filename;
    unsigned int error = lodepng::decode(out, w, h, fullPath);
    if (error) {
        spdlog::error("Failed to load PNG '{}': {}", filename, lodepng_error_text(error));
        return false;
    }
    return true;
}

static bool LoadMergeRegistry(const char* dataDir, std::vector<uint8_t>& atlasPixels) {
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
            uint16_t compositeId = static_cast<uint16_t>(std::stoi(fields[0]));
            uint16_t baseTileId = static_cast<uint16_t>(std::stoi(fields[1]));
            uint16_t overlayTileId = static_cast<uint16_t>(std::stoi(fields[2]));

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
                    if (f.size() >= 4 && static_cast<uint16_t>(std::stoi(f[0])) == overlayTileId) {
                        if (!LoadPNGRaw(f[1], overlayPng, ovW, ovH)) { std::fclose(tf); continue; }
                        found = true; break;
                    }
                }
                std::fclose(tf);
                if (!found || overlayPng.empty()) { spdlog::warn("Overlay tile {} not found", overlayTileId); continue; }
            }

            uint8_t ovTx = s_srcPosX[overlayTileId];
            uint8_t ovTy = s_srcPosY[overlayTileId];
            uint8_t baseTx = s_tileAtlasX[baseTileId];
            uint8_t baseTy = s_tileAtlasY[baseTileId];

            uint8_t atlasTx = s_nextAtlasSlot % TextureAtlas::kDefaultTilesX;
            uint8_t atlasTy = s_nextAtlasSlot / TextureAtlas::kDefaultTilesX;

            s_tileAtlasX[compositeId] = atlasTx;
            s_tileAtlasY[compositeId] = atlasTy;

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

                    // Alpha compositing: src over
                    float a = oa / 255.0f;
                    uint8_t cr = static_cast<uint8_t>(or2 * a + br * (1.0f - a));
                    uint8_t cg = static_cast<uint8_t>(og * a + bg * (1.0f - a));
                    uint8_t cb = static_cast<uint8_t>(ob * a + bb * (1.0f - a));
                    uint8_t ca = static_cast<uint8_t>(oa + ba * (1.0f - a));

                    int dx = (atlasTy * tSz + py) * s_atlasW + (atlasTx * tSz + px);
                    atlasPixels[dx * 4 + 0] = cr;
                    atlasPixels[dx * 4 + 1] = cg;
                    atlasPixels[dx * 4 + 2] = cb;
                    atlasPixels[dx * 4 + 3] = ca;
                }
            }

            s_nextAtlasSlot++;
            spdlog::info("Merged tile {} = base {} + overlay {}", compositeId, baseTileId, overlayTileId);
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing merge row '{}': {}", lineStr, e.what());
        }
    }

    std::fclose(file);
    return true;
}

static bool LoadSourcePNG(const std::string& filename, std::vector<uint8_t>& atlasPixels,
                         int atlasW, int atlasH, const std::vector<uint16_t>& tileIds) {
    std::string fullPath = std::string("data/textures/") + filename;
    
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
    
    for (uint16_t id = 0; id < 256; ++id) {
        s_tileAtlasX[id] = id % kDefaultTilesX;
        s_tileAtlasY[id] = id / kDefaultTilesY;
        s_srcPosX[id] = id % kDefaultTilesX;
        s_srcPosY[id] = id / kDefaultTilesY;
        s_rotate[id] = 0;
    }
    
    const char* dataDir = "data";
    s_nextAtlasSlot = 0;
    LoadTextureRegistry(dataDir);
    
    LoadBlockFaceRegistry(dataDir);
    LoadItemIconRegistry(dataDir);
    
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
                        uint16_t tileId = static_cast<uint16_t>(std::stoi(fields[0]));
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
    
    for (const auto& filePair : fileToTileIds) {
        LoadSourcePNG(filePair.first, atlasPixels, s_atlasW, s_atlasH, filePair.second);
    }
    
    LoadMergeRegistry(dataDir, atlasPixels);
    
    // Mark atlas slots already filled by loaded source tiles + merge tiles
    bool slotUsed[256] = {};
    for (const auto& filePair : fileToTileIds) {
        for (uint16_t tileId: filePair.second) {
            uint8_t slot = s_tileAtlasY[tileId] * kDefaultTilesX + s_tileAtlasX[tileId];
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
}

bool TextureAtlas::IsTransparent(uint16_t blockId) {
    if (!s_initialized) return false;
    auto* entry = s_blockFaces.find(blockId);
    return entry ? entry->transparent : false;
}

UVRect TextureAtlas::GetUV(uint16_t blockId, int face) {
    if (!s_initialized || face < 0 || face >= 6) return {0, 0, 1, 1};
    auto* entry = s_blockFaces.find(blockId);
    if (!entry) return {0, 0, 1, 1};
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

void TextureAtlas::Shutdown() {
    s_initialized = false;
    if (bgfx::isValid(s_texture)) bgfx::destroy(s_texture);
    s_texture = BGFX_INVALID_HANDLE;
    s_blockFaces.clear();
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