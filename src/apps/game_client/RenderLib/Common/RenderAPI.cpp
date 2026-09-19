
#define GLFW_EXPOSE_NATIVE_X11
#include "RenderAPI.h"
#include "IMeshProvider.h"
#include "IMinimapDataProvider.h"
#include "../UI/ImGuiBackend.h"
#include "../UI/Minimap.h"
#include "../Scene/RenderScene.h"
#include "../Utils/TextureAtlas.h"
#include <bgfx/bgfx.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#include <oneapi/tbb/concurrent_queue.h>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <X11/Xlib.h>
extern "C" {
GLFWAPI Display* glfwGetX11Display(void);
GLFWAPI Window glfwGetX11Window(GLFWwindow* window);
}
namespace renderlib {

// ========== Внутренние глобальные данные ==========
static GLFWwindow* s_window = nullptr;
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_ready{false};
static std::thread s_renderThread;
static std::mutex s_readyMutex;
static std::condition_variable s_readyCv;
static std::mutex s_frameMutex;
static std::condition_variable s_frameCond;
static bool s_frameProcessed = false;

static tbb::concurrent_bounded_queue<FrameRenderData> s_frameQueue;
static std::shared_ptr<IMeshProvider> s_meshProvider;
static std::shared_ptr<IMinimapDataProvider> s_minimapProvider;
static ImGuiDrawFn s_imguiCallback;

// Внутренние классы (скрыты в cpp)
class InternalRenderBackend {
public:
    void Init(void* ndt, void* nwh, int w, int h, const std::string& shaderDir);
    void Shutdown();
    void ProcessFrame(const FrameRenderData& frame);
    bool IsReady() const { return m_ready; }
private:
    bool m_ready = false;
    ImGuiBackend m_imgui;
    Minimap m_minimap;
    // Shaders загружаются один раз
    bool m_shadersLoaded = false;
    void LoadShaders(const std::string& shaderDir);
};

static InternalRenderBackend* s_backend = nullptr;

void InternalRenderBackend::LoadShaders(const std::string& shaderDir) {
    if (!RenderScene::LoadShaders(shaderDir)) {
        spdlog::error("RenderAPI: failed to load block shaders from {}", shaderDir);
    } else {
        spdlog::info("RenderAPI: block shaders loaded from {}", shaderDir);
    }
    m_shadersLoaded = true;
}

void InternalRenderBackend::Init(void* ndt, void* nwh, int w, int h, const std::string& shaderDir) {
    bgfx::PlatformData pd{};
    pd.ndt = ndt;
    pd.nwh = nwh;
    bgfx::Init init{};
    init.type = bgfx::RendererType::OpenGL;
    init.resolution.width = static_cast<uint32_t>(w);
    init.resolution.height = static_cast<uint32_t>(h);
    init.debug = true;
    init.platformData = pd;
    init.limits.maxTransientVbSize = 128 * 1024 * 1024;
    init.limits.maxTransientIbSize = 8 * 1024 * 1024;

    if (!bgfx::init(init)) {
        spdlog::error("RenderAPI: bgfx::init failed");
        m_ready = false;
        return;
    }
    spdlog::info("RenderAPI: bgfx initialised");

    // Инициализируем атлас текстур
    TextureAtlas::Init(16);

    // Инициализируем ImGui
    m_imgui.Init(s_window, shaderDir);

    // Загружаем шейдеры блоков (через RenderScene::LoadShaders)
    LoadShaders(shaderDir);

    m_ready = true;
    spdlog::info("RenderAPI: backend ready");
}

void InternalRenderBackend::Shutdown() {
    RenderScene::ShutdownShaders();
    m_imgui.Shutdown();
    TextureAtlas::Shutdown();
    bgfx::shutdown();
    m_ready = false;
}

void InternalRenderBackend::ProcessFrame(const FrameRenderData& frame) {
    if (!m_ready) return;
    if (!s_meshProvider) return;

    // Обновляем ImGui
    m_imgui.NewFrame(frame.base.width, frame.base.height, frame.base.dt,
                     frame.base.mouseX, frame.base.mouseY, frame.base.scrollY,
                     frame.base.mouseButtons, frame.base.mouseCaptured,
                     frame.base.keys, frame.base.charCount, frame.base.charCodepoints);

    // Рендер сцены (блоковые меши) через выносной RenderScene
    {
        RenderScene rs;
        rs.Render(frame.base.viewMatrix, frame.base.projMatrix,
                  frame.base.width, frame.base.height,
                  s_meshProvider.get(),
                  nullptr,  // minimap — handled explicitly below
                  nullptr,  // minimapData
                  nullptr,  // imgui — handled explicitly below
                  frame.base);
    }

    // Client ImGui drawing (debug window, crosshair, etc.) — gets full FrameRenderData
    if (s_imguiCallback) {
        s_imguiCallback(frame);
    }

    // Minimap: обновляем текстуру и рисуем окно
    if (s_minimapProvider) {
        m_minimap.Render(s_minimapProvider.get());
    }
    m_minimap.DrawImGui(frame.base.width, frame.base.height);

    // Отрисовка ImGui (содержит debug окно, crosshair и т.д.)
    m_imgui.Render();

    // После рендера, но до bgfx::frame() уведомляем игровой поток (раннее уведомление)
    {
        std::lock_guard<std::mutex> lock(s_frameMutex);
        s_frameProcessed = true;
    }
    s_frameCond.notify_one();

    // Завершаем кадр
    bgfx::frame();
}

// ========== Потоковая функция ==========
static void RenderThreadFunc(void* ndt, void* nwh, int width, int height, const std::string& shaderDir) {
    s_backend = new InternalRenderBackend();
    s_backend->Init(ndt, nwh, width, height, shaderDir);
    {
        std::lock_guard<std::mutex> lock(s_readyMutex);
        s_ready = s_backend->IsReady();
        s_running.store(s_ready); // если инит не удался, останавливаем
    }
    s_readyCv.notify_one();

    while (s_running) {
        FrameRenderData frame;
        if (s_frameQueue.try_pop(frame)) {
            if (!s_running) break;
            s_backend->ProcessFrame(frame);
        } else {
            // Нет кадра – небольшой сон, чтобы не грузить CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    delete s_backend;
    s_backend = nullptr;
}

// ========== Публичный API ==========
bool RenderAPI::Init(GLFWwindow* window, int width, int height, const std::string& shaderDir) {
    if (s_running) return true;
    s_window = window;
    void* ndt = nullptr;
    void* nwh = nullptr;
    ndt = glfwGetX11Display();
    nwh = (void*)(uintptr_t)glfwGetX11Window(window);
    s_running = true;
    s_renderThread = std::thread(RenderThreadFunc, ndt, nwh, width, height, shaderDir);
    // Ждём готовности
    std::unique_lock<std::mutex> lock(s_readyMutex);
    s_readyCv.wait(lock, []{ return s_ready.load(); });
    return s_ready;
}

void RenderAPI::Shutdown() {
    s_running = false;
    // Пустой кадр для разблокировки
    s_frameQueue.push(FrameRenderData{});
    if (s_renderThread.joinable())
        s_renderThread.join();
    s_meshProvider.reset();
    s_minimapProvider.reset();
}

void RenderAPI::SetMeshProvider(std::shared_ptr<IMeshProvider> provider) {
    s_meshProvider = std::move(provider);
}

void RenderAPI::SetMinimapDataProvider(std::shared_ptr<IMinimapDataProvider> provider) {
    s_minimapProvider = std::move(provider);
}

void RenderAPI::SubmitFrame(const FrameRenderData& frame) {
    s_frameQueue.push(frame);
}

void RenderAPI::WaitForFrame() {
    std::unique_lock<std::mutex> lock(s_frameMutex);
    s_frameCond.wait(lock, []{ return s_frameProcessed; });
    s_frameProcessed = false;
}

bool RenderAPI::IsReady() {
    return s_ready;
}

void RenderAPI::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    bgfx::reset(static_cast<uint32_t>(width), static_cast<uint32_t>(height), BGFX_RESET_NONE);
}

void RenderAPI::SetImGuiDrawCallback(ImGuiDrawFn fn) {
    s_imguiCallback = std::move(fn);
}

} // namespace renderlib