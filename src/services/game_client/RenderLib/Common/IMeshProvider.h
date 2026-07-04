#pragma once

#include <bgfx/bgfx.h>
#include <functional>
#include <glm/glm.hpp>

namespace renderlib {

struct MeshGpuHandles {
  bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
  bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;
};

struct MeshDrawData {
  MeshGpuHandles handles;
  const void *cpuVertices = nullptr; // for transient path
  uint32_t numVertices = 0;
  const void *cpuIndices = nullptr;
  uint32_t numIndices = 0;
  uint32_t vertexSize = 0; // stride
  bool transparent = false;
};

struct Frustum; // forward declare (defined elsewhere)

// Провайдер мешей для рендера сцены
class IMeshProvider {
public:
  virtual ~IMeshProvider() = default;
  // Вызывает callback для каждого видимого меша (фрустум во view space или
  // world space – определим позже)
  virtual void ForEachVisibleMesh(
      const Frustum &frustum,
      std::function<void(const MeshDrawData &)> callback) const = 0;
};

} // namespace renderlib