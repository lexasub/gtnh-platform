#include "Camera.h"
#include "UI/Core/InputBinder.h"
#include <GLFW/glfw3.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include "../Common/InputState.h"
#include "../World/World.h"

void Camera::Init() {
    // Start looking at -Z (identity orientation gives forward = (0,0,-1))
    orient = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    pos = glm::vec3(256.0f, 80.0f, 224.0f);
    fov = 70.0f;
}

void Camera::SetBinder(const InputBinder* binder) {
    binder_ = binder;
    resolveActionKeys();
}

void Camera::resolveActionKeys() {
    keyFwd_      = binder_->GetHeldKey("FWD");
    keyBkwd_     = binder_->GetHeldKey("BKWD");
    keyFwdAlt_   = binder_->GetHeldKey("FWD_ALT");
    keyBkwdAlt_  = binder_->GetHeldKey("BKWD_ALT");
    keyLeft_     = binder_->GetHeldKey("LEFT");
    keyRight_    = binder_->GetHeldKey("RIGHT");
    keyLeftAlt_  = binder_->GetHeldKey("LEFT_ALT");
    keyRightAlt_ = binder_->GetHeldKey("RIGHT_ALT");
    keyAscend_   = binder_->GetHeldKey("ASCEND");
    keyDescend_  = binder_->GetHeldKey("DESCEND");
}

void Camera::Update(float dt, const InputState& input) {
    // Mouse look
    float yawDelta = -static_cast<float>(input.mouseDX) * MOUSE_SENS;
    float pitchDelta = -static_cast<float>(input.mouseDY) * MOUSE_SENS;

    glm::quat yawRot = glm::angleAxis(glm::radians(yawDelta), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::quat pitchRot = glm::angleAxis(glm::radians(pitchDelta), glm::vec3(1.0f, 0.0f, 0.0f));
    orient = yawRot * orient * pitchRot;
    orient = glm::normalize(orient);

    // Zoom
    fov = glm::clamp(
        fov - static_cast<float>(input.scrollY) * ZOOM_SENS,
        10.0f,
        120.0f
    );

    // Movement (configurable via held bindings in bindings.json)
    float speed = SPEED * dt;
    float forward_primary = static_cast<float>(input.keys[keyFwd_]) - static_cast<float>(input.keys[keyBkwd_]);
    float forward_secondary = static_cast<float>(input.keys[keyFwdAlt_]) - static_cast<float>(input.keys[keyBkwdAlt_]);

    float right_primary = static_cast<float>(input.keys[keyRight_]) - static_cast<float>(input.keys[keyLeft_]);
    float right_secondary = static_cast<float>(input.keys[keyRightAlt_]) - static_cast<float>(input.keys[keyLeftAlt_]);

    // If primary is nonzero, use it; otherwise fall back to secondary
    float forward = forward_primary + (1.0f - glm::abs(forward_primary)) * forward_secondary;
    float right = right_primary + (1.0f - glm::abs(right_primary)) * right_secondary;

    if (!flightEnabled_ && world_) {
        // ── SURVIVAL / ADVENTURE: physics-based movement ──────────────────
        // Project movement vectors onto XZ plane (no vertical from camera angle)
        glm::vec3 fwdFlat = GetForward();
        fwdFlat.y = 0.0f;
        float fwdLen2 = glm::length2(fwdFlat);
        if (fwdLen2 > 0.0001f) fwdFlat /= std::sqrt(fwdLen2);

        glm::vec3 rightFlat = GetRight();
        rightFlat.y = 0.0f;
        float rightLen2 = glm::length2(rightFlat);
        if (rightLen2 > 0.0001f) rightFlat /= std::sqrt(rightLen2);

        // Gravity
        velocityY_ -= GRAVITY * dt;

        // Jump
        if (onGround_ && input.keys[keyAscend_]) {
            velocityY_ = JUMP_VELOCITY;
            onGround_ = false;
        }

        // Compute new position
        glm::vec3 move = (forward * fwdFlat + right * rightFlat) * speed;
        glm::vec3 newPos = pos + move;
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
        if (newPos.y <= groundY + EYE_HEIGHT) {
            newPos.y = groundY + EYE_HEIGHT;
            velocityY_ = 0.0f;
            onGround_ = true;
        } else {
            onGround_ = false;
        }

        // Simple block collision: check feet block and head block
        int fx = static_cast<int>(std::floor(newPos.x));
        int fy = static_cast<int>(std::floor(newPos.y - EYE_HEIGHT + 0.01f));
        int fz = static_cast<int>(std::floor(newPos.z));
        int hx = static_cast<int>(std::floor(newPos.x));
        int hy = static_cast<int>(std::floor(newPos.y + 0.3f));
        int hz = static_cast<int>(std::floor(newPos.z));

        bool blocked = world_->GetBlockAt(BlockPos{fx, fy, fz}) != 0 ||
                       world_->GetBlockAt(BlockPos{hx, hy, hz}) != 0;

        if (!blocked) {
            pos = newPos;
        } else {
            // Try X independently
            glm::vec3 testX = pos; testX.x = newPos.x;
            int txfx = static_cast<int>(std::floor(testX.x));
            if (world_->GetBlockAt(BlockPos{txfx, fy, fz}) == 0 &&
                world_->GetBlockAt(BlockPos{txfx, hy, hz}) == 0) {
                pos.x = newPos.x;
            }
            // Try Z independently
            glm::vec3 testZ = pos; testZ.z = newPos.z;
            int tzfz = static_cast<int>(std::floor(testZ.z));
            if (world_->GetBlockAt(BlockPos{fx, fy, tzfz}) == 0 &&
                world_->GetBlockAt(BlockPos{hx, hy, tzfz}) == 0) {
                pos.z = newPos.z;
            }
            // Always apply gravity (vertical is never blocked by walls)
            pos.y = newPos.y;
        }
    } else {
        // ── CREATIVE / SPECTATOR: free flight ─────────────────────────────
        float vertical = static_cast<float>(input.keys[keyAscend_]) - static_cast<float>(input.keys[keyDescend_]);
        pos += (forward * GetForward() + right * GetRight() + glm::vec3(0.0f, vertical, 0.0f)) * speed;
        velocityY_ = 0.0f;
        onGround_ = false;
    }
}

glm::mat4 Camera::GetViewMatrix() const {
    glm::mat4 rot = glm::mat4(orient);
    return glm::inverse(rot) * glm::translate(glm::mat4(1.0f), -pos);
}

glm::mat4 Camera::GetProjectionMatrix(float aspect) const {
    return glm::perspectiveRH(glm::radians(fov), aspect, NEAR_PLANE, FAR_PLANE);
}

glm::vec3 Camera::GetForward() const {
    return glm::normalize(orient * glm::vec3(0.0f, 0.0f, -1.0f));
}

glm::vec3 Camera::GetRight() const {
    return glm::normalize(orient * glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::vec3 Camera::GetUp() const {
    return glm::normalize(orient * glm::vec3(0.0f, 1.0f, 0.0f));
}

Frustum Camera::GetFrustum(float aspect) const {

    glm::mat4 view = GetViewMatrix();
    glm::mat4 proj = GetProjectionMatrix(aspect);
    glm::mat4 viewProj = proj * view;

    const float eps = 1e-6f;
    Frustum frustum;

    // Left   (col3 + col0)  = (m03+m00, m13+m10, m23+m20)
    frustum.planes[0].normal = glm::vec3(viewProj[0][3] + viewProj[0][0], viewProj[1][3] + viewProj[1][0], viewProj[2][3] + viewProj[2][0]);
    frustum.planes[0].distance = viewProj[3][3] + viewProj[3][0];
    // Right  (col3 - col0)
    frustum.planes[1].normal = glm::vec3(viewProj[0][3] - viewProj[0][0], viewProj[1][3] - viewProj[1][0], viewProj[2][3] - viewProj[2][0]);
    frustum.planes[1].distance = viewProj[3][3] - viewProj[3][0];
    // Bottom (col3 + col1)
    frustum.planes[2].normal = glm::vec3(viewProj[0][3] + viewProj[0][1], viewProj[1][3] + viewProj[1][1], viewProj[2][3] + viewProj[2][1]);
    frustum.planes[2].distance = viewProj[3][3] + viewProj[3][1];
    // Top    (col3 - col1)
    frustum.planes[3].normal = glm::vec3(viewProj[0][3] - viewProj[0][1], viewProj[1][3] - viewProj[1][1], viewProj[2][3] - viewProj[2][1]);
    frustum.planes[3].distance = viewProj[3][3] - viewProj[3][1];
    // Near   (col3 + col2)
    frustum.planes[4].normal = glm::vec3(viewProj[0][3] + viewProj[0][2], viewProj[1][3] + viewProj[1][2], viewProj[2][3] + viewProj[2][2]);
    frustum.planes[4].distance = viewProj[3][3] + viewProj[3][2];
    // Far    (col3 - col2)
    frustum.planes[5].normal = glm::vec3(viewProj[0][3] - viewProj[0][2], viewProj[1][3] - viewProj[1][2], viewProj[2][3] - viewProj[2][2]);
    frustum.planes[5].distance = viewProj[3][3] - viewProj[3][2];

    for (auto& plane : frustum.planes) {
        float len = glm::length(plane.normal);
        if (len > eps) {
            plane.normal /= len;
            plane.distance /= len;
        }
    }

    return frustum;
}