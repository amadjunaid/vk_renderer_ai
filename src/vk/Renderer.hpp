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
class SceneUniformBuffer;

/// The CPU-side mesh description an importer produces (see io/MeshLoader.hpp).
struct MeshData;

/// Owns everything that is needed to present frames: the mesh and its material,
/// the depth buffer, the framebuffers, the command pool and buffers, the
/// per-frame uniforms and the frame synchronisation objects.
class Renderer {
public:
    /// shaderDir is where the build put the compiled SPIR-V and dataDir where
    /// the model files live; both are absolute, so the working directory does
    /// not matter. enableDebugUi adds the ImGui menu bar, toolbar and model
    /// window; with it disabled the frame path stays a pure mesh render.
    Renderer(const VulkanContext& context, const Window& window, const std::string& shaderDir,
             const std::string& dataDir, bool enableDebugUi = true);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Runs the render loop until the window is closed.
    void run();

private:
    /// Frames the CPU may prepare while the GPU is still working on earlier ones.
    static constexpr std::uint32_t kMaxFramesInFlight = 2;

    /// The model loaded at startup, relative to the data directory. A missing
    /// file is not fatal: the renderer falls back to makeCubeMesh().
    static constexpr const char* kDefaultModelFile = "cube.glb";

    void drawFrame();

    void createCommandPool();
    void createFramebuffers();
    void destroyFramebuffers();
    void createDepthResources();
    void destroyDepthResources();
    void createCommandBuffers();
    void freeCommandBuffers();
    void createSyncObjects();
    void destroySyncObjects();

    /// Applies the Reset view request and this frame's mouse and wheel input to
    /// settings_.camera, the way Blender's viewport does: middle-drag orbits,
    /// Shift+middle-drag pans and the wheel (or Ctrl+middle-drag) zooms. On
    /// macOS, where a trackpad has no middle button, Option+drag stands in for
    /// it and Cmd works as the zoom modifier. Input that belongs to an ImGui
    /// window is left alone.
    void updateCamera();

    /// Fills the uniform buffer slice for `frame` with this frame's matrices,
    /// camera and light.
    void updateSceneUniforms(std::uint32_t frame);

    /// Reads a mesh file and puts it on screen. Any failure is reported through
    /// settings_; the mesh that is already loaded stays.
    void importMesh(const std::string& path);

    /// Uploads `data`, adopts its material and re-frames the camera on it.
    void applyMeshData(MeshData data, const std::string& source);

    void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex);
    void recreateSwapchain();

    const VulkanContext& context_;
    const Window& window_;

    /// Where the default model lives, handed in by main().
    const std::string dataDir_;

    /// Editable at runtime from the toolbar and the model window.
    RendererSettings settings_;

    std::unique_ptr<Swapchain> swapchain_;

    /// The mesh only borrows its material, so this has to be declared - and so
    /// destroyed - before it.
    std::unique_ptr<Material> material_;
    std::unique_ptr<Mesh> mesh_;

    /// Declared after material_, whose shaders it was built from.
    std::unique_ptr<GraphicsPipeline> pipeline_;

    /// Declared after pipeline_, whose descriptor set layout it is allocated
    /// from. That also puts it before the pipeline in the destruction order,
    /// which is the order the spec wants: the descriptor sets go first, then
    /// the layout they came from.
    std::unique_ptr<SceneUniformBuffer> sceneUniforms_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkFramebuffer> framebuffers_;

    /// The depth attachment. One image serves every framebuffer: only one
    /// render pass is recording into it at a time, and the pass clears it at the
    /// start of each frame.
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;

    /// Declared last so it is torn down before the render pass it points at.
    std::unique_ptr<DebugUi> debugUi_;

    /// Cursor position from the previous frame, so a drag turns into a delta.
    /// Tracked even while no button is down, so the first frame of a drag never
    /// starts with the jump from wherever the pointer last was.
    double lastCursorX_ = 0.0;
    double lastCursorY_ = 0.0;

    /// True while a middle-button drag that started in the viewport is in
    /// progress. A drag keeps steering the camera even when it wanders over an
    /// ImGui window, because letting go of the camera mid-gesture is worse than
    /// ignoring the panel it passes.
    bool navigating_ = false;

    /// The model transform is normalized so that any mesh - however big or off
    /// center the file made it - is centered on the origin with a bounding
    /// sphere of radius one. The camera's default pivot is the origin, so every
    /// import is framed the same way.
    float modelCenter_[3] = {0.0f, 0.0f, 0.0f};
    float modelScale_ = 1.0f;

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
