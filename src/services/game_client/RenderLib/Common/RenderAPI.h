#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct GLFWwindow;
namespace renderlib {
class IMeshProvider;
class IMinimapDataProvider;
struct FrameData;
struct FrameExt;
struct FrameRenderData;

// Callback for client-side ImGui drawing (Debug, Crosshair, etc.)
using ImGuiDrawFn = std::function<void(const FrameRenderData &frame)>;

// Основной API рендер-библиотеки (синглтон или свободные функции)
class RenderAPI {
public:
  // Инициализация: окно GLFW, размеры, папка с шейдерами
  static bool Init(GLFWwindow *window, int width, int height,
                   const std::string &shaderDir);
  static void Shutdown();

  // Передать данные кадра (матрицы, камера, ввод, highlight) и провайдеры
  // Провайдеры устанавливаются один раз при старте.
  static void SetMeshProvider(std::shared_ptr<IMeshProvider> provider);
  static void
  SetMinimapDataProvider(std::shared_ptr<IMinimapDataProvider> provider);
  static void SetImGuiDrawCallback(ImGuiDrawFn fn);

  // Вызывается каждый игровой тик (гейм-тред блокируется до завершения рендера
  // предыдущего кадра)
  static void SubmitFrame(const FrameRenderData &frame);
  static void WaitForFrame(); // optional

  // Для асинхронного управления (не обязателен, можно встроить в SubmitFrame)
  static bool IsReady();

  // Resize framebuffer when window size changes. Must be called from
  // the main thread (bgfx is in multithreaded mode, so this is safe between
  // frames).
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
  struct {
    int32_t x, y, z;
  } highlightedBlock{};
  uint16_t highlightedBlockId =
      0; // block ID at highlighted position, 0 if none
  bool hasHighlight = false;
  // Wrench guidance toast (server → client via ToolActionResp.message).
  // hudToastLifetime > 0 on the frame a new message is delivered; the overlay
  // re-arms its own fade timer from these fields.
  std::string hudToastText;
  float hudToastLifetime = 0.0f;
  // GT-style wrench overlay: drawn when the player holds a wrench and targets a
  // wrenchable block. wrenchConnectable[i] = side i (0=+X,1=-X,2=+Y,3=-Y,4=+Z,5=-Z)
  // has an adjacent pipe/cable the block can connect to.
  bool showWrenchOverlay = false;
  bool wrenchConnectable[6] = {false, false, false, false, false, false};
  bool showPipeFluidOverlay = false;
  bool pipeFluidConnectable[6] = {false, false, false, false, false, false};
  bool pipeFluidIsDense = false;
  bool pipeFluidOverlayOn = false;  // raw toggle state (for HUD indicator)
  std::string pipeFluidHoverInfo;   // what the crosshair targets (diagnostic HUD)
  int32_t pipeFluidAmount = 0;      // pipe fluid buffer level (mB)
  int32_t pipeFluidCapacity = 0;    // pipe fluid capacity (mB)
  uint32_t pipeFluidId = 0;         // 0 = empty
  // Wire face 0..5 the click ray ENTERS (the face the grid is drawn on),
  // 0xFF = none. Set by GameClient from the same RaycastHitAtCenter the click
  // handler uses, so the overlay's grid and the click's hit-test can never
  // pick different faces.
  uint8_t wrenchSideHit = 0xFF;
  uint16_t heldItemId = 0;
  size_t chunkCount = 0;
  size_t meshCount = 0;
};

// Complete frame packet
struct FrameRenderData {
  FrameData base;
  FrameExt ext;
};

} // namespace renderlib