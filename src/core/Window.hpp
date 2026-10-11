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

    /// Seconds elapsed since GLFW was initialized; drives animation.
    double elapsedSeconds() const { return glfwGetTime(); }

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

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* window_ = nullptr;

    /// Written by the GLFW callback, consumed by the render loop.
    mutable bool resized_ = false;
};
