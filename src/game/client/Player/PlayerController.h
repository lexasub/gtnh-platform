#pragma once

#include <glm/glm.hpp>

#include "../Common/Types.h"

class World;

// Aggregated movement intent, computed by the camera from the input bindings.
// Keeps the controller decoupled from GLFW / binding specifics.
struct PlayerMove {
  float forward = 0.0f;  // -1..1 (W/S)
  float right = 0.0f;    // -1..1 (A/D)
  float vertical = 0.0f; // fly only: up/down (Space/Shift)
  bool jump = false;     // walk only: ascend held
  bool sneak = false;    // walk only: descend held
};

// Owns the player body state (position, velocity, onGround) and implements
// per-mode physics. The camera renders from `pos` (eye position).
//
// - flight  (CREATIVE/SPECTATOR): free-fly, no gravity, no collision
// - walk    (SURVIVAL/ADVENTURE): gravity (~25 blocks/s²), jump, sneak,
//           axis-separated AABB sweep against solid blocks via World, with a
//           0.5-block step-up baked into the ground scan.
class PlayerController {
public:
  // Eye position — the camera renders from this.
  glm::vec3 pos{256.0f, 80.0f, 224.0f};

  void SetWorld(World *w) { world_ = w; }

  void Update(float dt, const PlayerMove &move, bool flight,
              glm::vec3 lookForward, glm::vec3 lookRight);

  bool IsOnGround() const { return onGround_; }

private:
  void UpdateFly(float dt, const PlayerMove &move, glm::vec3 lookForward,
                 glm::vec3 lookRight);
  void UpdateWalk(float dt, const PlayerMove &move, glm::vec3 lookForward,
                  glm::vec3 lookRight);

  World *world_ = nullptr;
  float velocityY_ = 0.0f;
  bool onGround_ = false;

  static constexpr float SPEED = 14.317f;
  static constexpr float GRAVITY = 25.0f;
  static constexpr float JUMP_VELOCITY = 8.5f;
  static constexpr float EYE_HEIGHT = 1.6f;
};
