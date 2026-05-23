#include "RenderScene.h"
#include "Frustum.h"
#include "../Utils/TextureAtlas.h"
#include "../UI/Minimap.h"
#include "../UI/ImGuiBackend.h"
#include <bgfx/bgfx.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>
#include <vector>

#include "Common/BlockVertex.h"
#include "RenderLib/Utils/ShaderLoader.h"

namespace renderlib {

static bgfx::ProgramHandle s_blockProgram = BGFX_INVALID_HANDLE;
static bgfx::UniformHandle s_u_texAtlas = BGFX_INVALID_HANDLE;
static bool s_shadersLoaded = false;

bool RenderScene::LoadShaders(const std::string& shaderDir) {
    std::string vsPath = shaderDir + "/vs_block.vert.bin";
    std::string fsPath = shaderDir + "/fs_block.frag.bin";
    const bgfx::Memory* vsMem = LoadBinaryShader(vsPath, "block shader");
    const bgfx::Memory* fsMem = LoadBinaryShader(fsPath, "block shader");
    if (!vsMem || !fsMem) return false;
    bgfx::ShaderHandle vs = bgfx::createShader(vsMem);
    bgfx::ShaderHandle fs = bgfx::createShader(fsMem);
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return false;
    }
    s_blockProgram = bgfx::createProgram(vs, fs, true);
    if (!bgfx::isValid(s_blockProgram)) {
        bgfx::destroy(vs); bgfx::destroy(fs);
        return false;
    }
    s_u_texAtlas = bgfx::createUniform("s_texAtlas", bgfx::UniformType::Sampler);
    s_shadersLoaded = true;
    return true;
}

void RenderScene::ShutdownShaders() {
    if (bgfx::isValid(s_blockProgram)) bgfx::destroy(s_blockProgram);
    if (bgfx::isValid(s_u_texAtlas)) bgfx::destroy(s_u_texAtlas);
    s_blockProgram = BGFX_INVALID_HANDLE;
    s_u_texAtlas = BGFX_INVALID_HANDLE;
    s_shadersLoaded = false;
}

void RenderScene::Render(const glm::mat4& viewMatrix, const glm::mat4& projMatrix,
                         int width, int height, IMeshProvider* meshProvider,
                         Minimap* minimap, IMinimapDataProvider* minimapData,
                         ImGuiBackend* imgui, const FrameData& /*base*/) {
    if (!meshProvider) return;

    // Setup view for geometry
    bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303030ff, 1.0f, 0);
    bgfx::setViewTransform(0, glm::value_ptr(viewMatrix), glm::value_ptr(projMatrix));
    bgfx::touch(0);

    // Build frustum
    Frustum frustum;
    glm::mat4 vp = projMatrix * viewMatrix;
    frustum.BuildFromViewProj(vp);

    // Collect visible meshes
    std::vector<MeshDrawData> visibleMeshes;
    meshProvider->ForEachVisibleMesh(frustum, [&](const MeshDrawData& data) {
        visibleMeshes.push_back(data);
    });

    if (!visibleMeshes.empty()) {
        // Ensure shaders and atlas are loaded
        if (!s_shadersLoaded) {
            return;
        }
        bgfx::setTexture(0, s_u_texAtlas, TextureAtlas::GetTextureHandle());
        bgfx::setState(BGFX_STATE_DEFAULT);

        // Try to merge all meshes into one transient buffer
        uint32_t totalVerts = 0, totalIndices = 0;
        for (auto& m : visibleMeshes) {
            if (m.cpuVertices) {
                totalVerts += m.numVertices;
                totalIndices += m.numIndices;
            }
        }
        bool merged = false;
        if (totalVerts > 0 && totalIndices > 0) {
            bgfx::TransientVertexBuffer tvb;
            bgfx::TransientIndexBuffer tib;
            if (bgfx::allocTransientBuffers(&tvb, BlockVertexLayout(), totalVerts, &tib, totalIndices, true)) {
                uint8_t* vertDst = tvb.data;
                uint8_t* idxDst = tib.data;
                uint32_t vertBase = 0;
                for (auto& m : visibleMeshes) {
                    if (!m.cpuVertices) continue;
                    uint32_t nv = m.numVertices;
                    uint32_t ni = m.numIndices;
                    memcpy(vertDst, m.cpuVertices, nv * m.vertexSize);
                    vertDst += nv * m.vertexSize;
                    const uint16_t* srcIdx = static_cast<const uint16_t*>(m.cpuIndices);
                    for (uint32_t i = 0; i < ni; ++i) {
                        *reinterpret_cast<uint32_t*>(idxDst) = srcIdx[i] + vertBase;
                        idxDst += sizeof(uint32_t);
                    }
                    vertBase += nv;
                }
                bgfx::setVertexBuffer(0, &tvb);
                bgfx::setIndexBuffer(&tib);
                bgfx::submit(0, s_blockProgram);
                merged = true;
            }
        }
        if (!merged) {
            // Fallback: submit each mesh individually
            for (auto& m : visibleMeshes) {
                if (bgfx::isValid(m.handles.vb) && bgfx::isValid(m.handles.ib)) {
                    bgfx::setVertexBuffer(0, m.handles.vb);
                    bgfx::setIndexBuffer(m.handles.ib);
                    bgfx::submit(0, s_blockProgram);
                }
            }
        }
    }

    // Render minimap if provided
    if (minimap && minimapData) {
        minimap->Render(minimapData);
        // Minimap ImGui drawing is handled by the game code, not here.
    }

    // Render ImGui
    if (imgui) {
        imgui->Render();
    }
}

} // namespace renderlib