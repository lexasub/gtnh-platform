#include "Window.h"
#include <stdexcept>

GLFWWindow::~GLFWWindow() {
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

bool GLFWWindow::Init(int width, int height, const char* title) {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, KeyCallback);
    glfwSetCharCallback(window_, CharCallback);
    glfwSetCursorPosCallback(window_, MouseMoveCallback);
    glfwSetMouseButtonCallback(window_, MouseButtonCallback);
    glfwSetScrollCallback(window_, ScrollCallback);

    return true;
}

void GLFWWindow::GlfwWaitEventsTimeout() { glfwWaitEventsTimeout(0.010); }

void GLFWWindow::KeyCallback(GLFWwindow* w, int key, int scancode, int action, int mods) {
    auto self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(w));
    if (self && self->onKey) self->onKey(key, scancode, action, mods);
}

void GLFWWindow::MouseMoveCallback(GLFWwindow* w, double x, double y) {
    auto self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(w));
    if (self && self->onMouseMove) self->onMouseMove(x, y);
}

void GLFWWindow::MouseButtonCallback(GLFWwindow* w, int button, int action, int mods) {
    auto self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(w));
    if (self && self->onMouseButton) self->onMouseButton(button, action, mods);
}

void GLFWWindow::ScrollCallback(GLFWwindow* w, double xoffset, double yoffset) {
    auto self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(w));
    if (self && self->onScroll) self->onScroll(xoffset, yoffset);
}

void GLFWWindow::CharCallback(GLFWwindow* w, unsigned int codepoint) {
    auto self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(w));
    if (self && self->onChar) self->onChar(codepoint);
}