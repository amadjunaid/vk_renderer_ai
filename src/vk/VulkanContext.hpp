#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <vector>

class Window;

/// Index of the queue families the renderer needs.
struct QueueFamilyIndices {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;

    bool isComplete() const { return graphics.has_value() && present.has_value(); }
};

/// What the surface supports on a given physical device.
struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;

    bool isAdequate() const { return !formats.empty() && !presentModes.empty(); }
};

/// Owns the "global" Vulkan objects: instance, debug messenger, surface,
/// physical device, logical device and the graphics/present queues.
///
/// Everything below the swapchain (render pass, pipeline, command buffers) is
/// owned by other classes so that swapchain recreation stays cheap and local.
class VulkanContext {
public:
    VulkanContext(const Window& window, bool enableValidation = true);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkSurfaceKHR surface() const { return surface_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }

    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkQueue presentQueue() const { return presentQueue_; }

    std::uint32_t graphicsFamily() const { return graphicsFamily_; }
    std::uint32_t presentFamily() const { return presentFamily_; }

    QueueFamilyIndices queryQueueFamilies(VkPhysicalDevice device) const;
    SwapchainSupport querySwapchainSupport(VkPhysicalDevice device) const;

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(const Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();

    std::vector<const char*> requiredInstanceExtensions() const;
    int scorePhysicalDevice(VkPhysicalDevice device) const;
    static bool supportsRequiredDeviceExtensions(VkPhysicalDevice device);
    static bool checkValidationLayerSupport();

    static constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    std::uint32_t graphicsFamily_ = 0;
    std::uint32_t presentFamily_ = 0;

    bool validationEnabled_ = false;
};
