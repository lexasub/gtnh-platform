#pragma once

#include "Common/InputState.h"
#include "Window.h"

// Owns the per-frame InputState + mouse capture toggle + first-frame guard.
// Subscribes GLFW callbacks on a GLFWWindow; call ResetFrameState() at end
// of every frame to clear transient deltas.
class InputManager {
public:
    InputState& State() { return input_; }
    const InputState& State() const { return input_; }

    // Sets onKey/onMouseMove/onMouseButton/onScroll callbacks on window.
    // Call once after window.Init().
    void Subscribe(GLFWWindow& window);

    void ResetFrameState() { input_.ResetFrameState(); }

    bool IsMouseCaptured() const { return mouseCaptured_; }
    void SetMouseCaptured(bool captured);

    bool IsFirstFrame() const { return firstFrame_; }
    void ClearFirstFrame() { firstFrame_ = false; }

private:
    GLFWwindow* windowHandle_ = nullptr;
    InputState input_;
    bool mouseCaptured_ = true;
    bool firstFrame_ = true;
};
