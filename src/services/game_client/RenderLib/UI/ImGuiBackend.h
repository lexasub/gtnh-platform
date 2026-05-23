#pragma once

#include <bgfx/bgfx.h>
#include <GLFW/glfw3.h>
#include <string>

struct ImDrawData;

namespace renderlib {

    class ImGuiBackend {
    public:
        ImGuiBackend();
        ~ImGuiBackend();
        void Init(GLFWwindow* window, const std::string& shaderDir);
        void Shutdown();
        void NewFrame(int width, int height, float dt,
                      double mouseX, double mouseY, double scrollY,
                      const bool mouseButtons[3], bool mouseCaptured,
                      const std::array<bool, 512>& keys,
                      int charCount, const uint32_t* charCodepoints);
        void Render();
    private:
        void CreateFontTexture();
        void RenderDrawData(ImDrawData* drawData);
        bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
        bgfx::VertexLayout layout_;
        bgfx::DynamicVertexBufferHandle vb_ = BGFX_INVALID_HANDLE;
        bgfx::DynamicIndexBufferHandle ib_ = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle fontTexture_ = BGFX_INVALID_HANDLE;
        bool initialized_ = false;
    };

} // namespace renderlib