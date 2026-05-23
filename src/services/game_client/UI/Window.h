#pragma once

#include <GLFW/glfw3.h>
#include <functional>

class GLFWWindow {
public:
    ~GLFWWindow();

    bool Init(int width, int height, const char* title);
    void GlfwWaitEventsTimeout();//FPS glfwPollEvents();

    bool ShouldClose() const { return glfwWindowShouldClose(window_); }
    void GetSize(int& width, int& height) const { glfwGetFramebufferSize(window_, &width, &height); }
    GLFWwindow* Handle() const { return window_; }

    // Callbacks (set before Init)
    std::function<void(int key, int scancode, int action, int mods)> onKey;
    std::function<void(double x, double y)> onMouseMove;
    std::function<void(int button, int action, int mods)> onMouseButton;
    std::function<void(double xoffset, double yoffset)> onScroll;
    std::function<void(unsigned int codepoint)> onChar;

private:
    GLFWwindow* window_ = nullptr;

    static void KeyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void MouseMoveCallback(GLFWwindow* w, double x, double y);
    static void MouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void ScrollCallback(GLFWwindow* w, double xoffset, double yoffset);
    static void CharCallback(GLFWwindow* w, unsigned int codepoint);
};