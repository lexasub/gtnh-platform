#pragma once

#include "../Common/IMeshProvider.h"
#include "../Common/RenderAPI.h"
#include "../UI/ImGuiBackend.h"
#include "../UI/Minimap.h"
#include "Frustum.h"
#include <glm/glm.hpp>
#include <string>

namespace renderlib {

class RenderScene {
public:
  // Загружает шейдеры блоков. Должен быть вызван один раз после bgfx::init.
  static bool LoadShaders(const std::string &shaderDir);
  static void ShutdownShaders();

  // Рендер блоковой сцены. Если minimap/imgui == nullptr — соответствующие шаги
  // пропускаются.
  void Render(const glm::mat4 &viewMatrix, const glm::mat4 &projMatrix,
              int width, int height, IMeshProvider *meshProvider,
              Minimap *minimap, IMinimapDataProvider *minimapData,
              ImGuiBackend *imgui, const FrameData &frame);
};

} // namespace renderlib