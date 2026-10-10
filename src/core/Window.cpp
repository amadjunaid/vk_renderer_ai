#include "core/Window.hpp"

#include "vk/VkCheck.hpp"

#include <iostream>
#include <stdexcept>

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
