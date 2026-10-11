#pragma once

#include "vk/DebugUi.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class VulkanContext;
class Window;
class Swapchain;
class GraphicsPipeline;
class Material;
class Mesh;

/// Owns everything that is needed to present frames: the mesh and its material,
/// the framebuffers, the command pool and buffers, and the frame synchronisation
/// objects.
class Renderer {
public:
    /// shaderDir is where the build put the compiled SPIR-V. enableDebugUi adds
    /// the ImGui menu bar and toolbar; with it disabled the frame path stays a
    /// pure mesh render.
    Renderer(const VulkanContext& context, const Window& window, const std::string& shaderDir,
             bool enableDebugUi = true);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Runs the render loop until the window is closed.
    void run();

private:
    /// Frames the CPU may prepare while the GPU is still working on earlier ones.
    static constexpr std::uint32_t kMaxFramesInFlight = 2;

    void drawFrame();

    void createCommandPool();
    void createFramebuffers();
    void destroyFramebuffers();
    void createCommandBuffers();
    void freeCommandBuffers();
    void createSyncObjects();
    void destroySyncObjects();

    /// Advances rotationAngle_ using the time since the previous frame, so
    /// pausing and resuming the spin does not make it jump.
    void advanceRotation();

    void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);
    void recreateSwapchain();

    const VulkanContext& context_;
    const Window& window_;

    /// Editable at runtime from the toolbar.
    RendererSettings settings_;

    std::unique_ptr<Swapchain> swapchain_;

    /// The mesh only borrows its material, so this has to be declared - and so
    /// destroyed - before it.
    std::unique_ptr<Material> material_;
    std::unique_ptr<Mesh> mesh_;

    /// Declared after material_, whose shaders it was built from.
    std::unique_ptr<GraphicsPipeline> pipeline_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkFramebuffer> framebuffers_;

    /// Declared last so it is torn down before the render pass it points at.
    std::unique_ptr<DebugUi> debugUi_;

    /// Current spin angle, kept in [0, 2pi) to stay precise over long runs.
    float rotationAngle_ = 0.0f;
    double previousFrameTime_ = 0.0;

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
