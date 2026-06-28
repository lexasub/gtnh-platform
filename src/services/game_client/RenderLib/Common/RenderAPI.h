#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <array>
#include <string>
#include <functional>
#include <cstdint>

struct GLFWwindow;
namespace renderlib {
    class IMeshProvider;
    class IMinimapDataProvider;
    struct FrameData;
    struct FrameExt;
    struct FrameRenderData;

    // Callback for client-side ImGui drawing (Debug, Crosshair, etc.)
    using ImGuiDrawFn = std::function<void(const FrameRenderData& frame)>;

    // Основной API рендер-библиотеки (синглтон или свободные функции)
    class RenderAPI {
    public:
        // Инициализация: окно GLFW, размеры, папка с шейдерами
        static bool Init(GLFWwindow* window, int width, int height, const std::string& shaderDir);
        static void Shutdown();

        // Передать данные кадра (матрицы, камера, ввод, highlight) и провайдеры
        // Провайдеры устанавливаются один раз при старте.
        static void SetMeshProvider(std::shared_ptr<IMeshProvider> provider);
        static void SetMinimapDataProvider(std::shared_ptr<IMinimapDataProvider> provider);
        static void SetImGuiDrawCallback(ImGuiDrawFn fn);

        // Вызывается каждый игровой тик (гейм-тред блокируется до завершения рендера предыдущего кадра)
        static void SubmitFrame(const FrameRenderData& frame);
        static void WaitForFrame(); // optional

        // Для асинхронного управления (не обязателен, можно встроить в SubmitFrame)
        static bool IsReady();

        // Resize framebuffer when window size changes. Must be called from
        // the main thread (bgfx is in multithreaded mode, so this is safe between frames).
        static void Resize(int width, int height);

    private:
        RenderAPI() = delete;
    };

    // Base frame data — consumed by RenderLib internals (scene, imgui backend)
    struct FrameData {
        static constexpr int kMaxInputChars = 16;
        glm::mat4 viewMatrix;
        glm::mat4 projMatrix;
        glm::vec3 cameraPos;
        int width = 1280, height = 720;
        float dt = 0.0f;
        double mouseX = 0.0, mouseY = 0.0;
        double scrollY = 0.0;
        bool mouseButtons[3] = {};
        bool mouseCaptured = false;
        std::array<bool, 512> keys{};
        uint32_t charCodepoints[kMaxInputChars] = {};
        int charCount = 0;
    };

    // Client extensions — passed through to ImGuiDrawFn for debug overlay
    struct FrameExt {
        struct { int32_t x, y, z; } highlightedBlock{};
        uint16_t highlightedBlockId = 0;   // block ID at highlighted position, 0 if none
        bool hasHighlight = false;
        size_t chunkCount = 0;
        size_t meshCount = 0;
    };

    // Complete frame packet
    struct FrameRenderData {
        FrameData base;
        FrameExt ext;
    };

} // namespace renderlib