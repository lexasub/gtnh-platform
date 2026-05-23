#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../Common/Types.h"
struct InputState;

class Camera {
public:
    void Init();
    void Update(float dt, const InputState& input);

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

    static constexpr float NEAR_PLANE = 0.1f;
    static constexpr float FAR_PLANE = 1000.0f;
    static constexpr float SPEED = 14.317f;
    static constexpr float MOUSE_SENS = 0.1f;
    static constexpr float ZOOM_SENS = 0.1f;
};
