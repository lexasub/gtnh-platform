#pragma once

#include "RenderLib/Common/RenderAPI.h"
#include <memory>

class World;
class Camera;
struct InputState;
class GLFWWindow;
struct BlockPos;
class MinimapWorldAdapter;
class UIManager; // NEW

// Thin bridge between GameClient subsystems and RenderLib.
// Owns the MinimapWorldAdapter, sets providers on RenderAPI::Init,
// builds FrameRenderData each frame, and installs the ImGui overlay
// callback (debug window, crosshair, block highlight wireframe, UI windows).
class RenderBridge {
public:
  explicit RenderBridge(World *world);
  ~RenderBridge();

  RenderBridge(const RenderBridge &) = delete;
  RenderBridge &operator=(const RenderBridge &) = delete;

  // NEW: Set the UIManager whose windows will be rendered in the ImGui overlay.
  void SetUIManager(UIManager *mgr) { uiMgr_ = mgr; }

  // Init RenderAPI, register providers + ImGui callback.
  void Init(GLFWwindow *window, int width, int height,
            const std::string &shaderDir);

  void Shutdown();

  void SetCameraPosition(const glm::vec3 &pos);

  // Assemble a FrameRenderData packet from current frame state.
  static renderlib::FrameRenderData
  BuildFrameData(const Camera &camera, const InputState &input, int width,
                 int height, float dt, bool mouseCaptured, bool hasHighlight,
                 BlockPos highlightedBlock, uint16_t highlightedBlockId,
                 size_t chunkCount, size_t meshCount);

  // Hit-test the GT-style wrench overlay bars (the per-face connection
  // markers) for a highlighted block.  Returns the overlay face index
  // {0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z} whose bar contains the mouse
  // position (window pixels), or -1 if the mouse is over no bar — the
  // caller then falls back to the raycast face.
  static int HitTestWrenchBar(const glm::mat4 &view, const glm::mat4 &proj,
                              int width, int height, const glm::vec3 &camPos,
                              const BlockPos &hb, double mouseX, double mouseY);

  void SubmitFrame(const renderlib::FrameRenderData &frd);
  void WaitForFrame();

private:
  // ImGui draw callback — renders HUD elements and delegates
  // all game UI windows to UIManager.
  static void ImGuiOverlay(const renderlib::FrameRenderData &frame);

  std::unique_ptr<MinimapWorldAdapter> mmProvider_;
  UIManager *uiMgr_ = nullptr; // NEW — not owned, set by GameClient
};
