#include "Camera.h"
#include <GLFW/glfw3.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include "../Common/InputState.h"

void Camera::Init() {
    // Start looking at -Z (identity orientation gives forward = (0,0,-1))
    orient = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    pos = glm::vec3(256.0f, 80.0f, 224.0f);
    fov = 70.0f;
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

    // Movement
    float speed = SPEED * dt;
    float forward_primary = static_cast<float>(input.keys[GLFW_KEY_W]) - static_cast<float>(input.keys[GLFW_KEY_S]);
    float forward_secondary = static_cast<float>(input.keys[GLFW_KEY_UP]) - static_cast<float>(input.keys[GLFW_KEY_DOWN]);

    float right_primary = static_cast<float>(input.keys[GLFW_KEY_D]) - static_cast<float>(input.keys[GLFW_KEY_A]);
    float right_secondary = static_cast<float>(input.keys[GLFW_KEY_RIGHT]) - static_cast<float>(input.keys[GLFW_KEY_LEFT]);

    // Если forward_primary == 0, то (1.0f - abs(forward_primary)) ≈ 1.0f
    // Иначе — близко к 0.0f (если |forward_primary| ≥ 1)
    float forward = forward_primary + (1.0f - glm::abs(forward_primary)) * forward_secondary;
    float right = right_primary + (1.0f - glm::abs(right_primary)) * right_secondary;
    pos += (forward * GetForward() + right * GetRight() + glm::vec3(
        0.0f,
        static_cast<float>(input.keys[GLFW_KEY_SPACE]) - static_cast<float>(input.keys[GLFW_KEY_LEFT_SHIFT]),
        0.0f
    )) * speed;
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