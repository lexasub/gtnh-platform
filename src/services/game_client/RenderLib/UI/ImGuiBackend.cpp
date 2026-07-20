#include "ImGuiBackend.h"
#include "../Utils/ShaderLoader.h"
#include <bgfx/defines.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <GLFW/glfw3.h>

namespace {

ImGuiKey GLFWKeyToImGuiKey(int key) {
    switch (key) {
        case GLFW_KEY_TAB: return ImGuiKey_Tab;
        case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
        case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow;
        case GLFW_KEY_UP: return ImGuiKey_UpArrow;
        case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
        case GLFW_KEY_PAGE_UP: return ImGuiKey_PageUp;
        case GLFW_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
        case GLFW_KEY_HOME: return ImGuiKey_Home;
        case GLFW_KEY_END: return ImGuiKey_End;
        case GLFW_KEY_INSERT: return ImGuiKey_Insert;
        case GLFW_KEY_DELETE: return ImGuiKey_Delete;
        case GLFW_KEY_BACKSPACE: return ImGuiKey_Backspace;
        case GLFW_KEY_SPACE: return ImGuiKey_Space;
        case GLFW_KEY_ENTER: return ImGuiKey_Enter;
        case GLFW_KEY_ESCAPE: return ImGuiKey_Escape;
        case GLFW_KEY_KP_ENTER: return ImGuiKey_KeypadEnter;
        case GLFW_KEY_A: return ImGuiKey_A;
        case GLFW_KEY_B: return ImGuiKey_B;
        case GLFW_KEY_C: return ImGuiKey_C;
        case GLFW_KEY_D: return ImGuiKey_D;
        case GLFW_KEY_E: return ImGuiKey_E;
        case GLFW_KEY_F: return ImGuiKey_F;
        case GLFW_KEY_G: return ImGuiKey_G;
        case GLFW_KEY_H: return ImGuiKey_H;
        case GLFW_KEY_I: return ImGuiKey_I;
        case GLFW_KEY_J: return ImGuiKey_J;
        case GLFW_KEY_K: return ImGuiKey_K;
        case GLFW_KEY_L: return ImGuiKey_L;
        case GLFW_KEY_M: return ImGuiKey_M;
        case GLFW_KEY_N: return ImGuiKey_N;
        case GLFW_KEY_O: return ImGuiKey_O;
        case GLFW_KEY_P: return ImGuiKey_P;
        case GLFW_KEY_Q: return ImGuiKey_Q;
        case GLFW_KEY_R: return ImGuiKey_R;
        case GLFW_KEY_S: return ImGuiKey_S;
        case GLFW_KEY_T: return ImGuiKey_T;
        case GLFW_KEY_U: return ImGuiKey_U;
        case GLFW_KEY_V: return ImGuiKey_V;
        case GLFW_KEY_W: return ImGuiKey_W;
        case GLFW_KEY_X: return ImGuiKey_X;
        case GLFW_KEY_Y: return ImGuiKey_Y;
        case GLFW_KEY_Z: return ImGuiKey_Z;
        case GLFW_KEY_0: return ImGuiKey_0;
        case GLFW_KEY_1: return ImGuiKey_1;
        case GLFW_KEY_2: return ImGuiKey_2;
        case GLFW_KEY_3: return ImGuiKey_3;
        case GLFW_KEY_4: return ImGuiKey_4;
        case GLFW_KEY_5: return ImGuiKey_5;
        case GLFW_KEY_6: return ImGuiKey_6;
        case GLFW_KEY_7: return ImGuiKey_7;
        case GLFW_KEY_8: return ImGuiKey_8;
        case GLFW_KEY_9: return ImGuiKey_9;
        case GLFW_KEY_LEFT_CONTROL: return ImGuiKey_LeftCtrl;
        case GLFW_KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
        case GLFW_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
        case GLFW_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
        case GLFW_KEY_LEFT_ALT: return ImGuiKey_LeftAlt;
        case GLFW_KEY_RIGHT_ALT: return ImGuiKey_RightAlt;
        case GLFW_KEY_LEFT_SUPER: return ImGuiKey_LeftSuper;
        case GLFW_KEY_RIGHT_SUPER: return ImGuiKey_RightSuper;
        case GLFW_KEY_F1: return ImGuiKey_F1;
        case GLFW_KEY_F2: return ImGuiKey_F2;
        case GLFW_KEY_F3: return ImGuiKey_F3;
        case GLFW_KEY_F4: return ImGuiKey_F4;
        case GLFW_KEY_F5: return ImGuiKey_F5;
        case GLFW_KEY_F6: return ImGuiKey_F6;
        case GLFW_KEY_F7: return ImGuiKey_F7;
        case GLFW_KEY_F8: return ImGuiKey_F8;
        case GLFW_KEY_F9: return ImGuiKey_F9;
        case GLFW_KEY_F10: return ImGuiKey_F10;
        case GLFW_KEY_F11: return ImGuiKey_F11;
        case GLFW_KEY_F12: return ImGuiKey_F12;
        case GLFW_KEY_KP_0: return ImGuiKey_Keypad0;
        case GLFW_KEY_KP_1: return ImGuiKey_Keypad1;
        case GLFW_KEY_KP_2: return ImGuiKey_Keypad2;
        case GLFW_KEY_KP_3: return ImGuiKey_Keypad3;
        case GLFW_KEY_KP_4: return ImGuiKey_Keypad4;
        case GLFW_KEY_KP_5: return ImGuiKey_Keypad5;
        case GLFW_KEY_KP_6: return ImGuiKey_Keypad6;
        case GLFW_KEY_KP_7: return ImGuiKey_Keypad7;
        case GLFW_KEY_KP_8: return ImGuiKey_Keypad8;
        case GLFW_KEY_KP_9: return ImGuiKey_Keypad9;
        case GLFW_KEY_CAPS_LOCK: return ImGuiKey_CapsLock;
        case GLFW_KEY_NUM_LOCK: return ImGuiKey_NumLock;
        case GLFW_KEY_PRINT_SCREEN: return ImGuiKey_PrintScreen;
        case GLFW_KEY_PAUSE: return ImGuiKey_Pause;
        case GLFW_KEY_MENU: return ImGuiKey_Menu;
        default: return ImGuiKey_None;
    }
}

// Thread-local reusable staging buffers to minimize per-frame allocations
thread_local std::vector<uint8_t> t_stagingVertexBuffer;
thread_local std::vector<uint8_t> t_stagingIndexBuffer;
} // anonymous namespace

namespace renderlib {


inline uint16_t ClipRectClamp(float v) {
    if (v < 0.0f) return 0;
    if (v > static_cast<float>(UINT16_MAX)) return UINT16_MAX;
    return static_cast<uint16_t>(v);
}

ImGuiBackend::ImGuiBackend() = default;
ImGuiBackend::~ImGuiBackend() { Shutdown(); }

void ImGuiBackend::Init(GLFWwindow* /*window*/, const std::string& shaderDir) {
    if (initialized_) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    std::string vsPath = shaderDir + "/vs_imgui.vert.bin";
    std::string fsPath = shaderDir + "/fs_imgui.frag.bin";
    const bgfx::Memory* vsMem = LoadBinaryShader(vsPath, "ImGui shader");
    const bgfx::Memory* fsMem = LoadBinaryShader(fsPath, "ImGui shader");
    if (!vsMem || !fsMem) {
        spdlog::warn("ImGui shaders not found, trying block shaders");
        vsPath = shaderDir + "/vs_block.vert.bin";
        fsPath = shaderDir + "/fs_block.frag.bin";
        vsMem = LoadBinaryShader(vsPath, "ImGui shader");
        fsMem = LoadBinaryShader(fsPath, "ImGui shader");
        if (!vsMem || !fsMem) { spdlog::error("Failed to load any shaders for ImGui"); return; }
    }
    bgfx::ShaderHandle vs = bgfx::createShader(vsMem);
    bgfx::ShaderHandle fs = bgfx::createShader(fsMem);
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
        if (bgfx::isValid(vs)) bgfx::destroy(vs);
        if (bgfx::isValid(fs)) bgfx::destroy(fs);
        return;
    }
    program_ = bgfx::createProgram(vs, fs, true);
    if (!bgfx::isValid(program_)) { bgfx::destroy(vs); bgfx::destroy(fs); return; }
    sampler_ = bgfx::createUniform("s_texAtlas", bgfx::UniformType::Sampler);
    layout_.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    vb_ = bgfx::createDynamicVertexBuffer(65536, layout_, BGFX_BUFFER_NONE);
    ib_ = bgfx::createDynamicIndexBuffer(131072, BGFX_BUFFER_NONE);
    if (!bgfx::isValid(vb_) || !bgfx::isValid(ib_)) {
        if (bgfx::isValid(vb_)) bgfx::destroy(vb_);
        if (bgfx::isValid(ib_)) bgfx::destroy(ib_);
        return;
    }
    CreateFontTexture();
    initialized_ = true;
    spdlog::info("ImGuiBackend initialized");
}

void ImGuiBackend::Shutdown() {
    if (!initialized_) return;
    if (bgfx::isValid(program_)) bgfx::destroy(program_);
    if (bgfx::isValid(sampler_)) bgfx::destroy(sampler_);
    if (bgfx::isValid(vb_)) bgfx::destroy(vb_);
    if (bgfx::isValid(ib_)) bgfx::destroy(ib_);
    if (bgfx::isValid(fontTexture_)) bgfx::destroy(fontTexture_);
    ImGui::DestroyContext();
    initialized_ = false;
}

void ImGuiBackend::NewFrame(int width, int height, float dt,
                             double mouseX, double mouseY, double scrollY,
                             const bool mouseButtons[3], bool,
                             const std::array<bool, 512>& keys,
                             int charCount, const uint32_t* charCodepoints) {
    if (!initialized_) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = dt;
    io.MousePos = ImVec2(static_cast<float>(mouseX), static_cast<float>(mouseY));
    for (int i = 0; i < 3; ++i) io.MouseDown[i] = mouseButtons[i];
    io.MouseWheel = static_cast<float>(scrollY);

    // Keyboard state
    for (int i = 0; i < 512; ++i) {
        ImGuiKey k = GLFWKeyToImGuiKey(i);
        if (k != ImGuiKey_None)
            io.AddKeyEvent(k, keys[i]);
    }

    // Character input
    for (int i = 0; i < charCount; ++i)
        io.AddInputCharacter(charCodepoints[i]);

    bgfx::setViewClear(1, BGFX_CLEAR_NONE);
    ImGui::NewFrame();
}

void ImGuiBackend::Render() {
    if (!initialized_) return;
    ImGui::Render();
    RenderDrawData(ImGui::GetDrawData());
}

void ImGuiBackend::RenderDrawData(ImDrawData* drawData) {
    if (!drawData || drawData->CmdListsCount == 0) return;
    uint16_t viewW = uint16_t(drawData->DisplaySize.x);
    uint16_t viewH = uint16_t(drawData->DisplaySize.y);
    bgfx::setViewRect(1, 0, 0, viewW, viewH);
    float ortho[16] = {
        2.0f / viewW, 0.0f, 0.0f, 0.0f,
        0.0f, -2.0f / viewH, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f
    };
    bgfx::setViewTransform(1, nullptr, ortho);
    bgfx::setScissor(0, 0, 0, 0);
    // Fallback path with index correction
    struct FallbackListInfo { uint32_t startVert, startIdx, numVerts, numIndices; const ImDrawList* list; };
    std::vector<FallbackListInfo> fallbackLists;
    uint32_t totalVerts = 0, totalIndices = 0;
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* list = drawData->CmdLists[n];
        uint32_t nv = list->VtxBuffer.Size, ni = list->IdxBuffer.Size;
        if (nv == 0 || ni == 0) continue;
        totalVerts += nv; totalIndices += ni;
        fallbackLists.push_back({totalVerts - nv, totalIndices - ni, nv, ni, list});
    }
    if (fallbackLists.empty()) return;
    // Reuse thread-local buffers; grow only if needed
    size_t vsize = totalVerts * sizeof(ImDrawVert);
    size_t isize = totalIndices * sizeof(ImDrawIdx);
    t_stagingVertexBuffer.reserve(vsize);
    t_stagingIndexBuffer.reserve(isize);
    uint8_t* stagingV = t_stagingVertexBuffer.data();
    uint8_t* stagingI = t_stagingIndexBuffer.data();
    for (auto& fb : fallbackLists) {
        memcpy(stagingV + fb.startVert * sizeof(ImDrawVert),
               fb.list->VtxBuffer.Data, fb.numVerts * sizeof(ImDrawVert));
        const ImDrawIdx* srcIdx = fb.list->IdxBuffer.Data;
        uint8_t* dstIdx = stagingI + fb.startIdx * sizeof(ImDrawIdx);
        for (uint32_t i = 0; i < fb.numIndices; ++i) {
            uint32_t newIdx = srcIdx[i] + fb.startVert;
            if (sizeof(ImDrawIdx) == 2) *((uint16_t*)dstIdx + i) = uint16_t(newIdx);
            else *((uint32_t*)dstIdx + i) = newIdx;
        }
    }
    bgfx::update(vb_, 0, bgfx::copy(stagingV, vsize));
    bgfx::update(ib_, 0, bgfx::copy(stagingI, isize));
    uint32_t globalIdxOffset = 0;
    for (auto& fb : fallbackLists) {
        uint32_t offset = 0;
        for (const ImDrawCmd& cmd : fb.list->CmdBuffer) {
            if (cmd.UserCallback) { cmd.UserCallback(fb.list, &cmd); continue; }
            bgfx::setVertexBuffer(0, vb_, 0, totalVerts);
            bgfx::setIndexBuffer(ib_, globalIdxOffset + offset, cmd.ElemCount);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                           BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA));
            bgfx::TextureHandle tex = fontTexture_;
            if (cmd.GetTexID()) tex.idx = static_cast<uint16_t>(cmd.GetTexID());
            bgfx::setTexture(0, sampler_, tex);
            bgfx::submit(1, program_);
            offset += cmd.ElemCount;
        }
        globalIdxOffset += fb.numIndices;
    }
}

void ImGuiBackend::CreateFontTexture() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    fontTexture_ = bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height),
                                         false, 1, bgfx::TextureFormat::RGBA8,
                                         BGFX_TEXTURE_NONE, bgfx::copy(pixels, width * height * 4));
    io.Fonts->TexID = static_cast<ImTextureID>(fontTexture_.idx);
}

} // namespace renderlib