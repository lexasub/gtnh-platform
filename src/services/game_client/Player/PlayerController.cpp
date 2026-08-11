#include "PlayerController.h"

#include "../World/World.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>

#include <cmath>

// Movement code ported verbatim from Camera::Update — the body physics now
// live here and the camera renders from PlayerController::pos.

void PlayerController::Update(float dt, const PlayerMove &move, bool flight,
                              glm::vec3 lookForward, glm::vec3 lookRight) {
    // Without a world there is nothing to collide with — fall back to flight.
    if (flight || !world_) {
        UpdateFly(dt, move, lookForward, lookRight);
    } else {
        UpdateWalk(dt, move, lookForward, lookRight);
    }
}

void PlayerController::UpdateFly(float dt, const PlayerMove &move,
                                 glm::vec3 lookForward,
                                 glm::vec3 lookRight) {
    float speed = SPEED * dt;
    pos += (move.forward * lookForward + move.right * lookRight +
            glm::vec3(0.0f, move.vertical, 0.0f)) *
           speed;
    velocityY_ = 0.0f;
    onGround_ = false;
}

void PlayerController::UpdateWalk(float dt, const PlayerMove &move,
                                  glm::vec3 lookForward,
                                  glm::vec3 lookRight) {
    // Project movement vectors onto XZ plane (no vertical from camera angle)
    glm::vec3 fwdFlat = lookForward;
    fwdFlat.y = 0.0f;
    float fwdLen2 = glm::length2(fwdFlat);
    if (fwdLen2 > 0.0001f) fwdFlat /= std::sqrt(fwdLen2);

    glm::vec3 rightFlat = lookRight;
    rightFlat.y = 0.0f;
    float rightLen2 = glm::length2(rightFlat);
    if (rightLen2 > 0.0001f) rightFlat /= std::sqrt(rightLen2);

    float speed = SPEED * dt;

    // Gravity
    velocityY_ -= GRAVITY * dt;

    // Jump / Sneak
    bool sneaking = move.sneak && onGround_;
    if (onGround_ && move.jump && !sneaking) {
        velocityY_ = JUMP_VELOCITY;
        onGround_ = false;
    }

    // Compute new position
    glm::vec3 moveVec = (move.forward * fwdFlat + move.right * rightFlat) * speed;
    glm::vec3 newPos = pos + moveVec;
    newPos.y += velocityY_ * dt;

    // Ground detection (scan no more than 12 blocks down)
    auto findGround = [this](float x, float z) -> float {
        int bx = static_cast<int>(std::floor(x));
        int bz = static_cast<int>(std::floor(z));
        int startY = static_cast<int>(std::floor(pos.y - EYE_HEIGHT));
        for (int y = startY; y >= 0 && y >= startY - 12; y--) {
            if (world_->GetBlockAt(BlockPos{bx, y, bz}) != 0) {
                return static_cast<float>(y + 1);
            }
        }
        return -1000.0f; // void — player falls
    };

    float groundY = findGround(newPos.x, newPos.z);

    // Sneak: stop at block edges — don't slide off
    if (sneaking) {
        float curGroundY = findGround(pos.x, pos.z);
        if (groundY < curGroundY && onGround_) {
            // Would step off a ledge — cancel horizontal movement
            newPos.x = pos.x;
            newPos.z = pos.z;
            groundY = curGroundY;
        }
    }

    float eyeHeight = sneaking ? EYE_HEIGHT - 0.3f : EYE_HEIGHT;
    if (newPos.y <= groundY + eyeHeight) {
        newPos.y = groundY + eyeHeight;
        velocityY_ = 0.0f;
        onGround_ = true;
    } else {
        onGround_ = false;
    }

    // Block collision: check all columns the player bounding box overlaps
    // Player width ≈ 0.6 blocks (±0.3 from center)
    static constexpr float kHalfWidth = 0.3f;
    auto blockAt = [this](int bx, int by, int bz) -> bool {
        return world_->GetBlockAt(BlockPos{bx, by, bz}) != 0;
    };
    auto collidesAt = [&](float px, float py, float pz) -> bool {
        int fy = static_cast<int>(std::floor(py - eyeHeight + 0.01f));
        int hy = static_cast<int>(std::floor(py + 0.3f));
        int x0 = static_cast<int>(std::floor(px - kHalfWidth));
        int x1 = static_cast<int>(std::floor(px + kHalfWidth));
        int z0 = static_cast<int>(std::floor(pz - kHalfWidth));
        int z1 = static_cast<int>(std::floor(pz + kHalfWidth));
        for (int ix = x0; ix <= x1; ++ix)
            for (int iz = z0; iz <= z1; ++iz)
                if (blockAt(ix, fy, iz) || blockAt(ix, hy, iz))
                    return true;
        return false;
    };

    if (!collidesAt(newPos.x, newPos.y, newPos.z)) {
        pos = newPos;
    } else {
        // Try X independently
        glm::vec3 testX = pos;
        testX.x = newPos.x;
        if (!collidesAt(testX.x, testX.y, testX.z)) {
            pos.x = newPos.x;
        }
        // Try Z independently
        glm::vec3 testZ = pos;
        testZ.z = newPos.z;
        if (!collidesAt(testZ.x, testZ.y, testZ.z)) {
            pos.z = newPos.z;
        }
        // Always apply gravity (vertical is never blocked by walls)
        pos.y = newPos.y;
    }
}
