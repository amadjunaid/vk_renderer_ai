#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>
#include <string>

/// Owns the GLFW library and the OS window.
///
/// Only the windowing concerns live here; the Vulkan objects that are bound to
/// the window (the surface) are created by VulkanContext / Swapchain so that
/// this class stays free of renderer state.
class Window {
public:
    Window(std::uint32_t width, std::uint32_t height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const { return glfwWindowShouldClose(window_) != 0; }
    void pollEvents() const { glfwPollEvents(); }

    /// Mouse-wheel movement accumulated since the previous call, in GLFW's
    /// units: positive when the wheel is rolled away from the user. Consuming
    /// it here rather than through a callback keeps the camera navigation with
    /// the code that owns the camera; the ImGui backend chains to this
    /// window's callback, so the value is captured even with the UI running.
    float consumeScrollDelta() const;

    /// Current drawable size in pixels. Zero while the window is minimized.
    VkExtent2D framebufferSize() const;

    /// Returns true once per framebuffer resize, then clears the flag.
    bool consumeResizeFlag() const {
        const bool resized = resized_;
        resized_ = false;
        return resized;
    }

    VkSurfaceKHR createSurface(VkInstance instance) const;

    /// The raw GLFW handle, needed by the ImGui GLFW backend.
    GLFWwindow* handle() const { return window_; }

    /// The OS window handle (an HWND on Windows, an NSWindow on macOS, null
    /// elsewhere). Native file dialogs take it so the dialog belongs to this
    /// window instead of floating behind it. Returned as void* so that no
    /// platform header has to leak into the renderer.
    void* nativeHandle() const;

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);

    GLFWwindow* window_ = nullptr;

    /// Written by the GLFW callback, consumed by the render loop.
    mutable bool resized_ = false;

    /// Wheel movement not yet read by consumeScrollDelta().
    mutable float scrollDelta_ = 0.0f;
};
