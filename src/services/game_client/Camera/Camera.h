#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../Common/Types.h"
#include "../Player/PlayerController.h"
#include <GLFW/glfw3.h>
#include <array>
struct InputState;
class InputBinder;

class World;

class Camera {
public:
  void Init();
  void Update(float dt, const InputState &input);
  void SetBinder(const InputBinder *binder);
  // Fly (CREATIVE/SPECTATOR) vs walk (SURVIVAL/ADVENTURE) — derives from the
  // permission matrix (GameModePerm::CanFly) in GameClient::Update.
  void SetFlightEnabled(bool enabled) { flightEnabled_ = enabled; }
  void SetWorld(World *w) { controller_.SetWorld(w); }

  glm::mat4 GetViewMatrix() const;
  glm::mat4 GetProjectionMatrix(float aspect) const;

  glm::vec3 GetRayOrigin() const { return pos; }
  glm::vec3 GetForward() const;
  glm::vec3 GetRight() const;
  glm::vec3 GetUp() const;

  bool IsOnGround() const { return controller_.IsOnGround(); }

  Frustum GetFrustum(float aspect) const;

  // Public parameters
  glm::vec3 pos{256.0f, 80.0f, 224.0f};
  float fov = 70.0f;

private:
  glm::quat orient{1.0f, 0.0f, 0.0f, 0.0f};

  void resolveActionKeys();

  const InputBinder *binder_ = nullptr;
  bool flightEnabled_ = true;

  // Body physics — position/velocity/onGround live here; `pos` mirrors the
  // controller's eye position each frame for the render path.
  PlayerController controller_;

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
  static constexpr float MOUSE_SENS = 0.1f;
  static constexpr float ZOOM_SENS = 0.1f;
};
