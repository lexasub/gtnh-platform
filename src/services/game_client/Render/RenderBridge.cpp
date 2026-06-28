#include "Render/RenderBridge.h"
#include "Render/MinimapWorldAdapter.h"
#include "World/World.h"
#include "Camera/Camera.h"
#include "Common/InputState.h"
#include "Common/Types.h"
#include "Crafting/ClientItemRegistry.h"
#include "UI/UIManager.h"
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

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
    for (auto& e : edges) {
        dl->AddLine(screen[e[0]], screen[e[1]],
                    IM_COL32(255, 255, 255, 220), 2.5f);
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
