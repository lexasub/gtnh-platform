#include "Render/RenderBridge.h"
#include "Render/WrenchOverlay.h"
#include "Render/MinimapWorldAdapter.h"
#include "World/World.h"
#include "Camera/Camera.h"
#include "Common/InputState.h"
#include "Common/Types.h"
#include "Crafting/ClientItemRegistry.h"
#include "UI/UIManager.h"
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

// Static pointer used by the ImGui overlay callback (static function).
// Set by RenderBridge::Init before registering the callback.
static UIManager* g_uiMgr = nullptr;

RenderBridge::RenderBridge(World* world) {
    mmProvider_ = std::make_unique<MinimapWorldAdapter>(world);
}

RenderBridge::~RenderBridge() {
    g_uiMgr = nullptr;
}

void RenderBridge::Init(GLFWwindow* window, int width, int height,
                         const std::string& shaderDir) {
    renderlib::RenderAPI::Init(window, width, height, shaderDir);

    renderlib::RenderAPI::SetMinimapDataProvider(
        std::shared_ptr<renderlib::IMinimapDataProvider>(
            mmProvider_.get(),
            [](void*){} /* no-op deleter — owned by RenderBridge */));

    // Sync static pointer for the static ImGuiOverlay callback
    g_uiMgr = uiMgr_;

    renderlib::RenderAPI::SetImGuiDrawCallback(&ImGuiOverlay);
}

void RenderBridge::Shutdown() {
    g_uiMgr = nullptr;
    mmProvider_.reset();
    renderlib::RenderAPI::Shutdown();
}

void RenderBridge::SetCameraPosition(const glm::vec3& pos) {
    if (mmProvider_) mmProvider_->SetCameraPosition(pos);
}

renderlib::FrameRenderData RenderBridge::BuildFrameData(
    const Camera& camera, const InputState& input,
    int width, int height, float dt, bool mouseCaptured,
    bool hasHighlight, BlockPos highlightedBlock,
    uint16_t highlightedBlockId,
    size_t chunkCount, size_t meshCount)
{
    auto frd = renderlib::FrameRenderData{
        .base = {
            .viewMatrix    = camera.GetViewMatrix(),
            .projMatrix    = camera.GetProjectionMatrix(
                static_cast<float>(width) / static_cast<float>(height)),
            .cameraPos     = camera.pos,
            .width         = width,
            .height        = height,
            .dt            = dt,
            .mouseX        = input.mouseX,
            .mouseY        = input.mouseY,
            .scrollY       = input.scrollY,
            .mouseButtons  = {input.mouseLeft, input.mouseRight, false},
            .mouseCaptured = mouseCaptured,
            .keys          = input.keys
        },
        .ext = {
            .highlightedBlock = {highlightedBlock.x, highlightedBlock.y,
                                 highlightedBlock.z},
            .highlightedBlockId = highlightedBlockId,
            .hasHighlight     = hasHighlight,
            .hudToastText     = {},
            .hudToastLifetime = 0.0f,
            .showWrenchOverlay = false,
            .wrenchConnectable = {false, false, false, false, false, false},
            .chunkCount       = chunkCount,
            .meshCount        = meshCount
        }
    };
    int copyCount = std::min(input.charCount, renderlib::FrameData::kMaxInputChars);
    for (int i = 0; i < copyCount; ++i)
        frd.base.charCodepoints[i] = input.charBuf[i];
    frd.base.charCount = copyCount;
    return frd;
}

void RenderBridge::SubmitFrame(const renderlib::FrameRenderData& frd) {
    renderlib::RenderAPI::SubmitFrame(frd);
}

void RenderBridge::WaitForFrame() {
    renderlib::RenderAPI::WaitForFrame();
}

int RenderBridge::HitTestWrenchBar(const glm::mat4& view, const glm::mat4& proj,
                                   int width, int height, const glm::vec3& camPos,
                                   const BlockPos& hb, double mouseX, double mouseY) {
    return wrench_overlay::HitTestWrenchBar(view, proj, width, height, camPos,
                                           hb, mouseX, mouseY);
}

// ---------------------------------------------------------------------------
// ImGui overlay — debug window, crosshair, block highlight wireframe + UI
// ---------------------------------------------------------------------------
void RenderBridge::ImGuiOverlay(const renderlib::FrameRenderData& frame) {
    // ---- Debug overlay window ----
    ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver); // запомнить позицию после первого появления
    ImGui::SetNextWindowSize(ImVec2(600, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Pos: %.1f %.1f %.1f", frame.base.cameraPos.x, frame.base.cameraPos.y, frame.base.cameraPos.z);
    ImGui::Text("Ch: %zu | Me: %zu", frame.ext.chunkCount, frame.ext.meshCount);
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("held=0x%04X wrenchOverlay=%d hlBlock=0x%04X",
                frame.ext.heldItemId, frame.ext.showWrenchOverlay ? 1 : 0,
                frame.ext.highlightedBlockId);
    ImGui::End();

    if (!g_uiMgr || !g_uiMgr->AnyOpen()) {
        int w = frame.base.width;
        int h = frame.base.height;
        ImVec2 center(w * 0.5f, h * 0.5f);
        const float barLen = 30.0f;
        const float barThick = 4.0f;
        const float gap = 4.0f;
        uint32_t col = IM_COL32(255, 255, 255, 240);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(center.x - barLen - gap, center.y - barThick),
                          ImVec2(center.x - gap, center.y + barThick), col);
        dl->AddRectFilled(ImVec2(center.x + gap, center.y - barThick),
                          ImVec2(center.x + barLen + gap, center.y + barThick), col);
        dl->AddRectFilled(ImVec2(center.x - barThick, center.y - barLen - gap),
                          ImVec2(center.x + barThick, center.y - gap), col);
        dl->AddRectFilled(ImVec2(center.x - barThick, center.y + gap),
                          ImVec2(center.x + barThick, center.y + barLen + gap), col);
    }

    // Wrench guidance toast (GT-style HUD message from the server).
    {
        static std::string sToastText;
        static float sToastTimer = 0.0f;
        if (frame.ext.hudToastLifetime > 0.0f && !frame.ext.hudToastText.empty()) {
            sToastText = frame.ext.hudToastText;
            sToastTimer = frame.ext.hudToastLifetime;
        }
        if (sToastTimer > 0.0f && !sToastText.empty()) {
            sToastTimer -= ImGui::GetIO().DeltaTime;
            if (sToastTimer < 0.0f) sToastTimer = 0.0f;
            const float alpha = std::min(1.0f, sToastTimer * 2.0f);
            ImGui::SetNextWindowPos(
                ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                       ImGui::GetIO().DisplaySize.y * 0.82f),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.55f * alpha);
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                     ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings;
            ImGui::Begin("WrenchToast", nullptr, flags);
            ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, alpha), "%s",
                               sToastText.c_str());
            ImGui::End();
        }
    }

    // ---- Block highlight wireframe ----
    if (!frame.ext.hasHighlight) {
        // ---- Game UI windows (inventory, workbench, machines, etc.) ----
        if (g_uiMgr) {
            g_uiMgr->RenderAll();
        }
        return;
    }
    const auto& hb = frame.ext.highlightedBlock;
    glm::vec3 corners[8] = {
        glm::vec3(hb.x,       hb.y,       hb.z),
        glm::vec3(hb.x + 1,   hb.y,       hb.z),
        glm::vec3(hb.x,       hb.y + 1,   hb.z),
        glm::vec3(hb.x + 1,   hb.y + 1,   hb.z),
        glm::vec3(hb.x,       hb.y,       hb.z + 1),
        glm::vec3(hb.x + 1,   hb.y,       hb.z + 1),
        glm::vec3(hb.x,       hb.y + 1,   hb.z + 1),
        glm::vec3(hb.x + 1,   hb.y + 1,   hb.z + 1),
    };

    glm::mat4 vp = frame.base.projMatrix * frame.base.viewMatrix;
    int sw = frame.base.width, sh = frame.base.height;
    ImVec2 screen[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec4 clip = vp * glm::vec4(corners[i], 1.0f);
        if (clip.w <= 0.0f) {
            screen[i] = ImVec2(-100, -100);
            continue;
        }
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        screen[i] = ImVec2((ndc.x * 0.5f + 0.5f) * sw,
                           (-ndc.y * 0.5f + 0.5f) * sh);
    }

    int edges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!frame.ext.showWrenchOverlay) {
        for (auto& e : edges) {
            dl->AddLine(screen[e[0]], screen[e[1]],
                        IM_COL32(255, 255, 255, 220), 2.5f);
        }
    }

    // ---- GT-style wrench overlay: faced-face outline + connectable side bars ----
    if (frame.ext.showWrenchOverlay) {
        // The grid is drawn on the face the click ray ENTERS (the near face) —
        // GameClient fills wrenchSideHit from the same RaycastHitAtCenter the
        // click handler uses, so the drawn grid and the click's hit-test can
        // never pick different faces. Fall back to the camera-facing face if
        // the side is unknown.
        int faced;
        if (frame.ext.wrenchSideHit < 6) {
            faced = wrench_grid::kWireToCube[frame.ext.wrenchSideHit];
        } else {
            const glm::vec3 blockCenter(hb.x + 0.5f, hb.y + 0.5f, hb.z + 0.5f);
            const glm::vec3 toBlock = blockCenter - frame.base.cameraPos;
            faced = 0;
            float best = 1e9f;
            for (int f = 0; f < 6; ++f) {
                const float d = glm::dot(wrench_overlay::kFaceNormal[f], toBlock);
                if (d < best) { best = d; faced = f; }
            }
        }

        // Faced-face outline (prominent).
        constexpr uint32_t faceCol = IM_COL32(255, 185, 40, 240);
        for (int e = 0; e < 4; ++e) {
            dl->AddLine(screen[wrench_overlay::kFaceCorners[faced][e]],
                        screen[wrench_overlay::kFaceCorners[faced][(e + 1) & 3]], faceCol, 2.5f);
        }

        // GTNH nine-grid: the grid is a 3x3 in the WORLD (u,v) axes of the
        // faced face (u/v per wrench_grid::uvAxes — the same axes
        // determineWrenchingSide consumes), projected to screen via the faced
        // face's corners. Each cell belongs to exactly one world face
        // (wrench_grid::zoneOf, matching GT5U GRID_SWITCH_TABLE): centre ->
        // faced, 4 edge cells -> side faces, 4 corners -> the far face.
        // Connected sides get an X at their cell.
        //
        // First order the faced face's projected corners canonically by world
        // (u,v): q[0]=(0,0) q[1]=(1,0) q[2]=(1,1) q[3]=(0,1). kFaceCorners[]
        // is not in that order for every face, so a straight lerp of fc[] (the
        // old code) mirrored the grid on +X/-X faces.
        const ImVec2 fc[4] = {
            screen[wrench_overlay::kFaceCorners[faced][0]],
            screen[wrench_overlay::kFaceCorners[faced][1]],
            screen[wrench_overlay::kFaceCorners[faced][2]],
            screen[wrench_overlay::kFaceCorners[faced][3]],
        };
        ImVec2 q[4];
        for (int i = 0; i < 4; ++i) {
            const glm::vec2 uv =
                wrench_grid::cornerUV(static_cast<uint8_t>(faced), wrench_overlay::kFaceCorners[faced][i]);
            const int col = uv.x > 0.5f ? 1 : 0;
            const int row = uv.y > 0.5f ? 1 : 0;
            const int qi = (row == 0 && col == 0) ? 0 : (row == 0) ? 1 : (col == 0) ? 3 : 2;
            q[qi] = fc[i];
        }
        // Bilinear map from world (u,v) on the faced face to screen.
        auto gridPoint = [&](float u, float v) -> ImVec2 {
            const float w0 = (1 - u) * (1 - v), w1 = u * (1 - v), w2 = u * v, w3 = (1 - u) * v;
            return ImVec2(q[0].x * w0 + q[1].x * w1 + q[2].x * w2 + q[3].x * w3,
                          q[0].y * w0 + q[1].y * w1 + q[2].y * w2 + q[3].y * w3);
        };
        // Interior grid: two vertical + two horizontal lines at 1/3 and 2/3.
        constexpr uint32_t gridCol = IM_COL32(255, 185, 40, 130);
        dl->AddLine(gridPoint(1.0f / 3.0f, 0.0f), gridPoint(1.0f / 3.0f, 1.0f), gridCol, 1.5f);
        dl->AddLine(gridPoint(2.0f / 3.0f, 0.0f), gridPoint(2.0f / 3.0f, 1.0f), gridCol, 1.5f);
        dl->AddLine(gridPoint(0.0f, 1.0f / 3.0f), gridPoint(1.0f, 1.0f / 3.0f), gridCol, 1.5f);
        dl->AddLine(gridPoint(0.0f, 2.0f / 3.0f), gridPoint(1.0f, 2.0f / 3.0f), gridCol, 1.5f);

        // Draw X marks in cells whose face is connected. wrenchConnectable[] is
        // indexed by META bit order {+X,-X,+Y,-Y,+Z,-Z} == cube-face order ==
        // wrench_grid face order — one index for all of them.
        const float cs = 7.0f;
        auto drawX = [&](const ImVec2& c) {
            dl->AddLine(ImVec2(c.x - cs, c.y - cs), ImVec2(c.x + cs, c.y + cs), IM_COL32(255, 190, 0, 255), 2.5f);
            dl->AddLine(ImVec2(c.x - cs, c.y + cs), ImVec2(c.x + cs, c.y - cs), IM_COL32(255, 190, 0, 255), 2.5f);
        };
        for (int m = 0; m < 6; ++m) {
            if (!frame.ext.wrenchConnectable[m]) continue;
            const uint8_t cell = wrench_grid::zoneOf(static_cast<uint8_t>(faced), static_cast<uint8_t>(m));
            if (cell == 0xFF) {
                for (int c : wrench_grid::kCornerCells) {
                    const glm::vec2 uv = wrench_grid::cellUV(static_cast<uint8_t>(c));
                    drawX(gridPoint(uv.x, uv.y));
                }
            } else {
                const glm::vec2 uv = wrench_grid::cellUV(cell);
                drawX(gridPoint(uv.x, uv.y));
            }
        }
    }

    // ---- Block name label (above highlighted block) ----
    if (frame.ext.highlightedBlockId != 0) {
        std::string_view name = ItemRegistry::GetName(frame.ext.highlightedBlockId);
        if (!name.empty()) {
            glm::vec3 labelPos(hb.x + 0.5f, hb.y + 1.3f, hb.z + 0.5f);
            glm::vec4 clip = vp * glm::vec4(labelPos, 1.0f);
            if (clip.w > 0.0f) {
                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                float sx = (ndc.x * 0.5f + 0.5f) * sw;
                float sy = (-ndc.y * 0.5f + 0.5f) * sh;

                ImVec2 textSize = ImGui::CalcTextSize(name.data(), name.data() + name.size());
                const float padding = 8.0f;
                ImVec2 bgMin(sx - textSize.x * 0.5f - padding, sy - textSize.y - padding);
                ImVec2 bgMax(sx + textSize.x * 0.5f + padding, sy + padding);

                dl->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 180), 6.0f);
                dl->AddRect(bgMin, bgMax, IM_COL32(255, 255, 255, 60), 6.0f);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.5f,
                            ImVec2(sx - textSize.x * 0.5f, sy - textSize.y),
                            IM_COL32(255, 255, 255, 255), name.data(), name.data() + name.size());
            }
        }
    }

    // ---- Game UI windows (inventory, workbench, machines, etc.) ----
    if (g_uiMgr) {
        g_uiMgr->RenderAll();
    }
}
