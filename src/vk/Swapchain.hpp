#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class VulkanContext;
class Window;

/// The swapchain and its image views.
///
/// Framebuffers are intentionally not owned here: they reference the render
/// pass, which belongs to GraphicsPipeline, so Renderer owns them instead and
/// rebuilds them whenever either side changes.
class Swapchain {
public:
    Swapchain(const VulkanContext& context, const Window& window);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /// Destroys and rebuilds the swapchain. The caller must have idled the
    /// device and released anything referencing the old images.
    void recreate(const Window& window);

    VkSwapchainKHR handle() const { return swapchain_; }
    VkFormat imageFormat() const { return imageFormat_; }
    VkExtent2D extent() const { return extent_; }

    const std::vector<VkImageView>& imageViews() const { return imageViews_; }
    std::uint32_t imageCount() const { return static_cast<std::uint32_t>(images_.size()); }

private:
    void createSwapchain(const Window& window);
    void createImageViews();
    void destroy();

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window& window) const;

    const VulkanContext& context_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
};
