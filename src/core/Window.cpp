#include "core/Window.hpp"

#include "vk/VkCheck.hpp"

#include <iostream>
#include <stdexcept>

// The native handle accessors live in glfw3native.h, which only declares the
// accessor for the platforms whose macro is defined. The X11/Wayland backends
// are deliberately left out: their accessors need the platform headers, and the
// only consumer (the file dialog) is Windows-only.
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

namespace {

void glfwErrorCallback(int code, const char* description) {
    std::cerr << "[glfw] error " << code << ": " << description << '\n';
}

}  // namespace

Window::Window(std::uint32_t width, std::uint32_t height, const std::string& title) {
    glfwSetErrorCallback(glfwErrorCallback);

    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    // Vulkan only: tell GLFW not to create an OpenGL context.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title.c_str(),
                               nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        throw std::runtime_error("Failed to create the GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    // Installed before the ImGui backend initializes, so its own scroll
    // callback chains to this one instead of replacing it.
    glfwSetScrollCallback(window_, scrollCallback);
}

Window::~Window() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}

void Window::framebufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
    // Only the fact that a resize happened matters here: the new size is read
    // with glfwGetFramebufferSize when the swapchain is rebuilt.
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self != nullptr) {
        self->resized_ = true;
    }
}

void Window::scrollCallback(GLFWwindow* window, double /*xOffset*/, double yOffset) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self != nullptr) {
        self->scrollDelta_ += static_cast<float>(yOffset);
    }
}

float Window::consumeScrollDelta() const {
    const float delta = scrollDelta_;
    scrollDelta_ = 0.0f;
    return delta;
}

void* Window::nativeHandle() const {
#if defined(_WIN32)
    return glfwGetWin32Window(window_);
#elif defined(__APPLE__)
    return glfwGetCocoaWindow(window_);
#else
    return nullptr;
#endif
}

VkExtent2D Window::framebufferSize() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);

    VkExtent2D extent{};
    extent.width = width > 0 ? static_cast<std::uint32_t>(width) : 0U;
    extent.height = height > 0 ? static_cast<std::uint32_t>(height) : 0U;
    return extent;
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = glfwCreateWindowSurface(instance, window_, nullptr, &surface);
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string("Failed to create the window surface (") +
                                 vkutil::resultToString(result) + ")");
    }
    return surface;
}
