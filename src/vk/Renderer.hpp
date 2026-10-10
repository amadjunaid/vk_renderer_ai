#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VulkanContext;
class Window;
class Swapchain;
class GraphicsPipeline;

/// Owns everything that is needed to present frames: framebuffers, the command
/// pool and buffers, and the frame synchronisation objects.
class Renderer {
public:
    Renderer(const VulkanContext& context, const Window& window, const std::string& shaderDir);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Runs the render loop until the window is closed.
    void run();

private:
    /// Frames the CPU may prepare while the GPU is still working on earlier ones.
    static constexpr std::uint32_t kMaxFramesInFlight = 2;

    /// Speed of the triangle's spin, in radians per second.
    static constexpr float kRotationSpeed = 0.75f;

    void drawFrame();

    void createCommandPool();
    void createFramebuffers();
    void destroyFramebuffers();
    void createCommandBuffers();
    void freeCommandBuffers();
    void createSyncObjects();
    void destroySyncObjects();

    void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);
    void recreateSwapchain();

    const VulkanContext& context_;
    const Window& window_;
    std::string shaderDir_;

    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<GraphicsPipeline> pipeline_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkFramebuffer> framebuffers_;

    /// Signalled when the swapchain image the CPU wants is ready.
    std::vector<VkSemaphore> imageAvailable_;
    /// Signalled when rendering into a swapchain image finished.
    std::vector<VkSemaphore> renderFinished_;
    /// One per frame in flight; tells the CPU a frame slot is reusable.
    std::vector<VkFence> inFlightFences_;
    /// Per swapchain image: the fence of the frame that last used that image,
    /// or VK_NULL_HANDLE. Needed because the image count is not necessarily a
    /// multiple of kMaxFramesInFlight.
    std::vector<VkFence> imagesInFlight_;

    std::uint32_t currentFrame_ = 0;
};
