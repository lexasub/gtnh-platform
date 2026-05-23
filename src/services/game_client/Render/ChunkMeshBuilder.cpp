#include "ChunkMeshBuilder.h"
#include "../World/ChunkView.h"
#include "../Cache/ChunkNeighborCache.h"
#include "../RenderLib/Utils/TextureAtlas.h"
#include "PipeMeshBuilder.h"
#include "CableMeshBuilder.h"
#include "BlockRenderRegistry.h"
#include <array>

namespace {
    thread_local ChunkMeshBuilder::MeshData data;

    /// Flat block color for MVP — no per-face brightness.
    /// Format: 0xAABBGGRR (same as atlas, extracted as R=byte0, G=byte1, B=byte2, A=byte3).
    static constexpr uint32_t GetBlockColor(uint16_t blockId) {
        switch (blockId) {
            case 1:  return 0xFF808080;  // stone - gray
            case 2:  return 0xFF2B5A8B;  // dirt - brown
            case 3:  return 0xFF50AF4C;  // grass - green
            default: return 0xFFFFFFFF;  // white
        }
    }
}

ChunkMeshBuilder::MeshData ChunkMeshBuilder::Build(const ChunkNeighborCache &cache, std::shared_ptr<const ChunkView> chunk) {
    // Faces: [0]=+X, [1]=-X, [2]=+Y, [3]=-Y, [4]=+Z, [5]=-Z
    static constexpr std::array<std::array<std::array<int,3>,4>,6> faces = {{
        {{{1,0,0},{1,1,0},{1,1,1},{1,0,1}}},  // +X
        {{{0,0,1},{0,1,1},{0,1,0},{0,0,0}}},  // -X
        {{{0,1,1},{1,1,1},{1,1,0},{0,1,0}}},  // +Y
        {{{0,0,0},{1,0,0},{1,0,1},{0,0,1}}},  // -Y
        {{{0,0,1},{1,0,1},{1,1,1},{0,1,1}}},  // +Z
        {{{1,0,0},{0,0,0},{0,1,0},{1,1,0}}}   // -Z
    }};

    static constexpr std::array<std::array<int8_t,3>,6> normals = {{
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
    }};

    static constexpr std::array<std::array<int8_t,3>,6> deltas = {{
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
    }};

    data.vertices.clear();
    data.indices.clear();

    if (chunk.use_count() == 0) {
        return data;
    }

    data.vertices.reserve(CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 8); //24/3 - 1/3 filled
    data.indices.reserve(CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 12); //36/3 - 1/3 filled


    uint16_t idx = 0; //TODO may be > uint16_t - 32x32x32 x 6x4 = 76 432 > 65 536

    for (int y = 0; y < CHUNK_SIZE; ++y) {
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                uint16_t block = chunk->GetBlock(x, y, z);
                if (block == 0) continue;

                if (isPipeBlock(block)) {
                    static PipeMeshBuilder pipeBuilder;
                    auto pipeType = blockIdToPipeType(block);
                    auto mask = pipeBuilder.detectConnections(x, y, z, pipeType,
                        [&](int32_t bx, int32_t by, int32_t bz) { return cache.GetBlock(bx, by, bz); });
                    auto pipeMesh = pipeBuilder.buildPipeMesh(x, y, z, pipeType, mask);
                    size_t vertBase = data.vertices.size();
                    data.vertices.insert(data.vertices.end(), pipeMesh.vertices.begin(), pipeMesh.vertices.end());
                    for (auto& idx : pipeMesh.indices)
                        data.indices.push_back(static_cast<uint16_t>(idx + vertBase));
                    continue;
                }

                if (isCableBlock(block)) {
                    static CableMeshBuilder cableBuilder;
                    auto tier = blockIdToCableTier(block);
                    auto mask = cableBuilder.detectConnections(x, y, z, tier,
                        [&](int32_t bx, int32_t by, int32_t bz) { return cache.GetBlock(bx, by, bz); });
                    auto cableMesh = cableBuilder.buildCableMesh(x, y, z, tier, mask);
                    size_t vertBase = data.vertices.size();
                    data.vertices.insert(data.vertices.end(), cableMesh.vertices.begin(), cableMesh.vertices.end());
                    for (auto& idx : cableMesh.indices)
                        data.indices.push_back(static_cast<uint16_t>(idx + vertBase));
                    continue;
                }

                for (int f = 0; f < 6; ++f) {
                    if (cache.GetBlock(x + deltas[f][0], y + deltas[f][1], z + deltas[f][2]) != 0)
                        continue;

                    const auto &face = faces[f];
                    auto uv = renderlib::TextureAtlas::GetUV(block & 0xFF, f);

                    auto nx = static_cast<uint8_t>((normals[f][0] * 0.5f + 0.5f) * 255.0f);
                    auto ny = static_cast<uint8_t>((normals[f][1] * 0.5f + 0.5f) * 255.0f);
                    auto nz = static_cast<uint8_t>((normals[f][2] * 0.5f + 0.5f) * 255.0f);

                    auto du = uv.u1 - uv.u0;
                    auto dv = uv.v1 - uv.v0;

                    const uint32_t c = GetBlockColor(block);

                    for (int v = 0; v < 4; ++v) {
                        data.vertices.push_back({
                            .x = static_cast<float>(x + face[v][0]),
                            .y = static_cast<float>(y + face[v][1]),
                            .z = static_cast<float>(z + face[v][2]),
                            .normal = {nx, ny, nz, 0},
                            .color = {
                                static_cast<uint8_t>((c >> 0) & 0xFF),
                                static_cast<uint8_t>((c >> 8) & 0xFF),
                                static_cast<uint8_t>((c >> 16) & 0xFF),
                                static_cast<uint8_t>((c >> 24) & 0xFF)
                            },
                            .u = uv.u0 + du * (v & 1),
                            .v = uv.v0 + dv * ((v >> 1) & 1)
                        });
                    }

                    data.indices.push_back(idx + 0);
                    data.indices.push_back(idx + 1);
                    data.indices.push_back(idx + 2);
                    data.indices.push_back(idx + 0);
                    data.indices.push_back(idx + 2);
                    data.indices.push_back(idx + 3);
                    idx += 4;
                }
            }
        }
    }
    data.vertices.shrink_to_fit();
    data.indices.shrink_to_fit();

    return std::move(data);
}
