#include "TextureAtlas.h"
#include <bgfx/bgfx.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <lodepng.h>
#include <spdlog/spdlog.h>

namespace renderlib {

UVRect TextureAtlas::s_uvCache[256][6] = {};

static bgfx::TextureHandle s_texture = BGFX_INVALID_HANDLE;
static int s_tileSize = 16;
static int s_atlasW = 256;
static int s_atlasH = 256;
static uint8_t s_tileX[256][6]{};
static uint8_t s_tileY[256][6]{};
static bool s_transparent[256]{};
static bool s_initialized = false;

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

static bool LoadTextureRegistry(const char* dataDir, uint8_t& nextAtlasSlot) {
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
            s_tileAtlasX[tileId] = nextAtlasSlot % TextureAtlas::kDefaultTilesX;
            s_tileAtlasY[tileId] = nextAtlasSlot / TextureAtlas::kDefaultTilesX;
            s_srcPosX[tileId] = tileX;
            s_srcPosY[tileId] = tileY;
            s_rotate[tileId] = rotate & 3;
            nextAtlasSlot++;
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
            uint16_t blockId = static_cast<uint16_t>(std::stoi(fields[0]));
            s_transparent[blockId] = (std::stoi(fields[7]) == 1);
            
            // Each face_* value is a tile_id; look up its atlas grid position
            static constexpr int kFaceFields[6] = {1, 2, 3, 4, 5, 6}; // fields[1..6] = px,nx,py,ny,pz,nz
            for (int f = 0; f < 6; ++f) {
                uint16_t tileId = static_cast<uint16_t>(std::stoi(fields[kFaceFields[f]]));
                s_tileX[blockId][f] = s_tileAtlasX[tileId];
                s_tileY[blockId][f] = s_tileAtlasY[tileId];
            }
        } catch (const std::exception& e) {
            spdlog::warn("Error parsing block face registry row '{}': {}", lineStr, e.what());
            continue;
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
    
    // Initialize position arrays with defaults
    for (uint16_t id = 0; id < 256; ++id) {
        s_tileAtlasX[id] = id % kDefaultTilesX;
        s_tileAtlasY[id] = id / kDefaultTilesY;
        s_srcPosX[id] = id % kDefaultTilesX;
        s_srcPosY[id] = id / kDefaultTilesY;
        s_transparent[id] = false;
        s_rotate[id] = 0;
    }
    
    // Load texture registry — auto-assigns atlas slots, tile_x/tile_y are source PNG only
    const char* dataDir = "data";
    uint8_t nextAtlasSlot = 0;
    LoadTextureRegistry(dataDir, nextAtlasSlot);
    
    // Set default block face mapping: block N uses tile N for all faces
    for (uint16_t id = 0; id < 256; ++id) {
        for (int f = 0; f < 6; ++f) {
            s_tileX[id][f] = s_tileAtlasX[id];  // tile N at its atlas position
            s_tileY[id][f] = s_tileAtlasY[id];
        }
    }
    
    // Load block face overrides from CSV
    LoadBlockFaceRegistry(dataDir);
    
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
    
    // Mark atlas slots already filled by loaded source tiles
    bool slotUsed[256] = {};
    for (const auto& filePair : fileToTileIds)
        for (uint16_t tileId : filePair.second) {
            uint8_t slot = s_tileAtlasY[tileId] * kDefaultTilesX + s_tileAtlasX[tileId];
            slotUsed[slot] = true;
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
    
    const bgfx::Memory* mem = bgfx::copy(atlasPixels.data(), static_cast<uint32_t>(atlasPixels.size()));
    s_texture = bgfx::createTexture2D(static_cast<uint16_t>(s_atlasW), static_cast<uint16_t>(s_atlasH),
                                      false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, mem);
    
    s_initialized = true;
}

bool TextureAtlas::IsTransparent(uint16_t blockId) {
    if (!s_initialized) return false;
    return s_transparent[blockId & 0xFF];
}

UVRect TextureAtlas::GetUV(uint16_t tileId, int face) {
    // tileId не должен выходить за 255, face < 6
    return s_uvCache[tileId][face];
}

void TextureAtlas::Shutdown() {
    s_initialized = false;
    if (bgfx::isValid(s_texture)) bgfx::destroy(s_texture);
    s_texture = BGFX_INVALID_HANDLE;
    std::memset(s_transparent, 0, sizeof(s_transparent));
    std::memset(s_tileX, 0, sizeof(s_tileX));
    std::memset(s_tileY, 0, sizeof(s_tileY));
    std::memset(s_uvCache, 0, sizeof(s_uvCache));
}

bgfx::TextureHandle TextureAtlas::GetTextureHandle() {
    return s_texture;
}

} // namespace renderlib