#include "vk/Swapchain.hpp"

#include "core/Window.hpp"
#include "vk/VulkanContext.hpp"
#include "vk/VkCheck.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

Swapchain::Swapchain(const VulkanContext& context, const Window& window) : context_(context) {
    createSwapchain(window);
    createImageViews();
}

Swapchain::~Swapchain() {
    destroy();
}

void Swapchain::destroy() {
    const VkDevice device = context_.device();

    for (const VkImageView view : imageViews_) {
        vkDestroyImageView(device, view, nullptr);
    }
    imageViews_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    images_.clear();
}

void Swapchain::recreate(const Window& window) {
    destroy();
    createSwapchain(window);
    createImageViews();
}

VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const {
    // Prefer 8-bit sRGB in BGRA order, which is what desktop compositors hand back.
    for (const VkSurfaceFormatKHR& format : available) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return available.front();
}

VkPresentModeKHR Swapchain::choosePresentMode(const std::vector<VkPresentModeKHR>& available) const {
    // FIFO (vsync) is guaranteed to exist; MAILBOX avoids a full CPU stall when
    // the queue is full, but only if the driver exposes it.
    const auto hasMailbox = std::any_of(available.begin(), available.end(), [](VkPresentModeKHR mode) {
        return mode == VK_PRESENT_MODE_MAILBOX_KHR;
    });
    return hasMailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities,
                                   const Window& window) const {
    // A width other than UINT32_MAX means the surface dictates the size.
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    const VkExtent2D actual = window.framebufferSize();
    const auto clamp = [](std::uint32_t value, std::uint32_t low, std::uint32_t high) {
        return std::max(low, std::min(value, high));
    };

    VkExtent2D extent{};
    extent.width = clamp(actual.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height =
        clamp(actual.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

void Swapchain::createSwapchain(const Window& window) {
    const SwapchainSupport support = context_.querySwapchainSupport(context_.physicalDevice());

    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
    extent_ = chooseExtent(support.capabilities, window);

    if (extent_.width == 0 || extent_.height == 0) {
        throw std::runtime_error("Cannot create a swapchain for a zero-sized window");
    }

    // One image more than the minimum so the driver is not forced to wait for
    // the presentation engine to release an image.
    std::uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = context_.surface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent_;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const std::uint32_t familyIndices[] = {context_.graphicsFamily(), context_.presentFamily()};
    if (familyIndices[0] != familyIndices[1]) {
        // The two queues live in different families, so the images are shared
        // concurrently instead of transferring ownership between frames.
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = familyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;  // recreate() destroys the old one first

    VK_CHECK(vkCreateSwapchainKHR(context_.device(), &createInfo, nullptr, &swapchain_));

    std::uint32_t actualImageCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(context_.device(), swapchain_, &actualImageCount, nullptr));
    images_.resize(actualImageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(context_.device(), swapchain_, &actualImageCount, images_.data()));

    imageFormat_ = surfaceFormat.format;
}

void Swapchain::createImageViews() {
    imageViews_.resize(images_.size());

    for (std::size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = images_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = imageFormat_;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(context_.device(), &createInfo, nullptr, &imageViews_[i]));
    }
}
