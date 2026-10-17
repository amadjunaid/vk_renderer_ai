#pragma once

#include "vk/Material.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>

class VulkanContext;
class Window;

/// Values the debug UI is allowed to change while the renderer is running, plus
/// the read-only facts it displays.
///
/// The renderer owns one of these and hands out a reference, so an edit is
/// visible to the very next frame with nothing to synchronise. The shading
/// parameters are not in here - they live in the Material, which the UI is
/// handed separately, because that is where the renderer reads them from.
struct RendererSettings {
    /// Spins the mesh around its vertical axis. The angle itself lives in the
    /// renderer; these two only drive it.
    bool rotate = true;
    float rotationSpeed = 0.75f;  // radians per second

    /// Orbit camera, in the renderer's world convention (+Z up, -X forward).
    /// Angles are radians: yaw swings around the up axis - 0 puts the eye on
    /// +X, and the default looks from -X, which is the side an imported mesh
    /// faces. The distance is in world units relative to a model that the
    /// renderer normalizes to a radius of one, so the default frames any
    /// imported mesh the same way.
    float cameraYaw = 2.3561945f;   // 135 degrees
    float cameraPitch = 0.35f;
    float cameraDistance = 3.0f;

    /// Raised by the Reset view button and cleared by the renderer once the
    /// camera is back at its defaults. A request rather than an action because
    /// the UI has no camera to reset - it only edits these three numbers.
    bool resetViewRequested = false;

    // --- Filled in by the renderer, displayed read-only ----------------------

    /// The name the loader found for the current mesh, and the file it came
    /// from (empty for the built-in fallback).
    std::string modelName;
    std::string modelSource;

    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;

    /// Outcome of the last import attempt, shown in the model window.
    std::string statusMessage;
    bool statusIsError = false;
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
    /// Called with the path the user picked under File > Import > Mesh. It runs
    /// inside the frame, so the renderer may load the file synchronously and
    /// report the result through RendererSettings before the next frame.
    using ImportCallback = std::function<void(const std::string&)>;

    DebugUi(const VulkanContext& context, const Window& window, VkRenderPass renderPass,
            std::uint32_t swapchainImageCount, RendererSettings& settings, Material& material,
            ImportCallback onImportMesh);
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
    void drawModelWindow();
    void drawAboutWindow();

    /// The File > Import submenu. Shows the native dialog when the platform has
    /// one and falls back to the path box in the model window otherwise.
    void drawImportMenu();

    const VulkanContext& context_;
    RendererSettings& settings_;

    /// Edited live by the model window, so the reference is held rather than
    /// copied: the renderer reads the same object back every frame.
    Material& material_;

    /// Invoked when the user picks a mesh to load.
    ImportCallback onImportMesh_;

    /// The window the file dialog should be owned by (see Window::nativeHandle).
    void* nativeWindow_ = nullptr;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    /// Only used where filedialog reports no native dialog is available.
    char importPath_[512] = {};

    bool quitRequested_ = false;
    bool showToolbar_ = true;
    bool showModelWindow_ = true;
    bool showDemoWindow_ = false;
    bool showAboutWindow_ = false;
};
