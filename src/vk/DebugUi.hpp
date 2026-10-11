#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

class VulkanContext;
class Window;

/// Values the debug UI is allowed to change while the renderer is running.
struct RendererSettings {
    /// Spin speed of the mesh, in radians per second.
    float rotationSpeed = 0.75f;
    /// Pauses the spin without stopping the render loop.
    bool rotate = true;
};

/// Dear ImGui menus and toolbar.
///
/// The UI is given GraphicsPipeline's render pass and draws into it, rather than
/// owning a pass of its own. That works because the Vulkan backend records draw
/// commands into whatever render pass is already active: it only ever calls
/// vkCmdBeginRenderPass from the ImGui_ImplVulkanH_* helpers, which this project
/// does not use. The payoff is that there are no ImGui framebuffers to rebuild
/// when the swapchain changes - only the pipeline, and only when the render pass
/// itself is replaced (i.e. when the swapchain format changes).
class DebugUi {
public:
    DebugUi(const VulkanContext& context, const Window& window, VkRenderPass renderPass,
            std::uint32_t swapchainImageCount, RendererSettings& settings);
    ~DebugUi();

    DebugUi(const DebugUi&) = delete;
    DebugUi& operator=(const DebugUi&) = delete;

    /// Opens a new ImGui frame. Call once per frame, before buildUi().
    void beginFrame();

    /// Emits the menu bar, the toolbar and any open windows.
    void buildUi();

    /// Records the UI. Must be called while renderPass() is active.
    void render(VkCommandBuffer commandBuffer) const;

    /// Resyncs ImGui after the swapchain was rebuilt. Takes the (possibly new)
    /// render pass, so it is safe to call even when the format did not change.
    void onSwapchainRecreated(VkRenderPass renderPass, std::uint32_t swapchainImageCount);

    /// The render pass the UI pipeline was built against.
    VkRenderPass renderPass() const { return renderPass_; }

    /// True once the user picked File > Quit.
    bool wantsQuit() const { return quitRequested_; }

private:
    void drawToolbar();
    void drawAboutWindow();

    const VulkanContext& context_;
    RendererSettings& settings_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    bool quitRequested_ = false;
    bool showToolbar_ = true;
    bool showDemoWindow_ = false;
    bool showAboutWindow_ = false;
};
