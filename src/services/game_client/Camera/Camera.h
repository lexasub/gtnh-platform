#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../Common/Types.h"
#include <GLFW/glfw3.h>
#include <array>
struct InputState;
class InputBinder;

class Camera {
public:
  void Init();
  void Update(float dt, const InputState &input);
  void SetBinder(const InputBinder *binder);

  glm::mat4 GetViewMatrix() const;
  glm::mat4 GetProjectionMatrix(float aspect) const;

  glm::vec3 GetRayOrigin() const { return pos; }
  glm::vec3 GetForward() const;
  glm::vec3 GetRight() const;
  glm::vec3 GetUp() const;

  Frustum GetFrustum(float aspect) const;

  // Public parameters
  glm::vec3 pos{256.0f, 80.0f, 224.0f};
  float fov = 70.0f;

private:
  glm::quat orient{1.0f, 0.0f, 0.0f, 0.0f};

  void resolveActionKeys();

  const InputBinder *binder_ = nullptr;
  // default
  int keyFwd_ = -1;
  int keyBkwd_ = -1;
  int keyFwdAlt_ = -1;
  int keyBkwdAlt_ = -1;
  int keyLeft_ = -1;
  int keyRight_ = -1;
  int keyLeftAlt_ = -1;
  int keyRightAlt_ = -1;
  int keyAscend_ = -1;
  int keyDescend_ = -1;

  static constexpr float NEAR_PLANE = 0.1f;
  static constexpr float FAR_PLANE = 1000.0f;
  static constexpr float SPEED = 14.317f;
  static constexpr float MOUSE_SENS = 0.1f;
  static constexpr float ZOOM_SENS = 0.1f;
};
