#include "InputManager.h"
#include <GLFW/glfw3.h>

void InputManager::Subscribe(GLFWWindow& window) {
    windowHandle_ = window.Handle();

    glfwSetInputMode(windowHandle_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    window.onKey = [this](int key, [[maybe_unused]] int scancode, int action, [[maybe_unused]] int mods) {
        if (key >= 0 && key < 512) input_.keys[key] = (action != GLFW_RELEASE);
        if (!firstFrame_ && action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) {
            mouseCaptured_ = !mouseCaptured_;
            glfwSetInputMode(windowHandle_, GLFW_CURSOR,
                             mouseCaptured_ ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        }
    };
    window.onMouseMove = [this](double x, double y) {
        input_.mouseDX = x - input_.mouseX;
        input_.mouseDY = y - input_.mouseY;
        input_.mouseX = x;
        input_.mouseY = y;
    };
    window.onMouseButton = [this](int button, int action, [[maybe_unused]] int mods) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            input_.mouseLeft = (action == GLFW_PRESS);
            input_.mouseLeftPressed = (action == GLFW_PRESS);
        } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            input_.mouseRight = (action == GLFW_PRESS);
            input_.mouseRightPressed = (action == GLFW_PRESS);
        }
    };
    window.onScroll = [this]([[maybe_unused]] double xoffset, double yoffset) {
        input_.scrollY += yoffset;
    };
    window.onChar = [this](unsigned int codepoint) {
        if (input_.charCount < InputState::kMaxCharsPerFrame)
            input_.charBuf[input_.charCount++] = codepoint;
    };

    // Re-assert capture after all callbacks registered
    mouseCaptured_ = true;
    glfwSetInputMode(windowHandle_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // Initialise mouseX/Y with the actual cursor position so the first
    // onMouseMove computes a correct (small) delta instead of a huge jump
    // from the default (0, 0).
    double sx, sy;
    glfwGetCursorPos(windowHandle_, &sx, &sy);
    input_.mouseX = sx;
    input_.mouseY = sy;
}

void InputManager::SetMouseCaptured(bool captured) {
    mouseCaptured_ = captured;
    glfwSetInputMode(windowHandle_, GLFW_CURSOR,
                     captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}
