#include "vk/Renderer.hpp"

#include "core/Math.hpp"
#include "core/Window.hpp"
#include "io/MeshLoader.hpp"
#include "vk/Buffer.hpp"
#include "vk/DebugUi.hpp"
#include "vk/GraphicsPipeline.hpp"
#include "vk/Material.hpp"
#include "vk/Mesh.hpp"
#include "vk/SceneUniformBuffer.hpp"
#include "vk/Swapchain.hpp"
#include "vk/VulkanContext.hpp"
#include "vk/VkCheck.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr float kTwoPi = 6.28318530718f;

/// The PBR shader pair every material is drawn with. Only the file names are
/// fixed here, because the directory holding the compiled SPIR-V is only known
/// at run time (the build passes it in). Importing a mesh never changes the
/// shaders, only the geometry and the shading parameters, so the pipeline built
/// from these stays valid for every file the user opens.
constexpr const char* kMaterialName = "pbr";
constexpr const char* kMaterialVertexShader = "mesh.vert.spv";
constexpr const char* kMaterialFragmentShader = "mesh.frag.spv";

/// Camera setup, in the renderer's world convention: +Z up, -X forward. The
/// field of view is the vertical one; the aspect ratio is read from the
/// swapchain every frame, so a resize needs no camera rebuild.
constexpr float kFieldOfView = 0.7853982f;  // 45 degrees
constexpr float kNearPlane = 0.05f;
constexpr float kFarPlane = 100.0f;

/// Pitch is an elevation above the horizon, clamped just below a right angle:
/// looking straight down would put the eye directly above the model, where the
/// up vector is parallel to the view direction and lookAt collapses.
constexpr float kMaxCameraPitch = 1.5533f;  // 89 degrees

/// The single directional light, in world space. It stays fixed rather than
/// following the camera, so the shading changes as the mesh spins and its shape
/// reads clearly. Mostly from above (+Z), tipped towards the -X/+Y side so the
/// faces the default camera sees are the lit ones. The intensity is folded into
/// the color.
constexpr float kLightDirection[3] = {-0.45f, 0.35f, 0.85f};
constexpr float kLightColor[3] = {2.2f, 2.1f, 2.0f};

/// True for the depth formats that also carry a stencil, which changes the
/// aspect mask the image view has to declare.
bool hasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

}  // namespace

Renderer::Renderer(const VulkanContext& context, const Window& window, const std::string& shaderDir,
                   const std::string& dataDir, bool enableDebugUi)
    : context_(context), window_(window), dataDir_(dataDir) {
    swapchain_ = std::make_unique<Swapchain>(context_, window_);

    material_ = std::make_unique<Material>(kMaterialName, shaderDir + "/" + kMaterialVertexShader,
                                           shaderDir + "/" + kMaterialFragmentShader);
    pipeline_ = std::make_unique<GraphicsPipeline>(context_, swapchain_->imageFormat(), *material_);
    sceneUniforms_ = std::make_unique<SceneUniformBuffer>(context_, pipeline_->descriptorSetLayout(),
                                                          kMaxFramesInFlight);

    // Start on the model the repository ships. A missing or broken data file is
    // deliberately not fatal: the renderer falls back to a built-in cube and
    // says so, which beats refusing to open a window over a missing asset.
    if (dataDir_.empty()) {
        applyMeshData(makeCubeMesh(), std::string());
        settings_.statusMessage = "Built-in cube";
    } else {
        const std::string defaultModel = dataDir_ + "/" + kDefaultModelFile;
        try {
            applyMeshData(loadMesh(defaultModel), defaultModel);
            settings_.statusMessage = "Loaded the default model";
        } catch (const std::exception& error) {
            applyMeshData(makeCubeMesh(), std::string());
            settings_.statusMessage =
                std::string("Built-in cube (the default model is unavailable: ") + error.what() + ")";
            settings_.statusIsError = true;
        }
    }

    createCommandPool();
    createDepthResources();
    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();

    if (enableDebugUi) {
        // Created after the pipeline because the UI renders into the pipeline's
        // render pass, and after the mesh because the model window displays what
        // the importer found.
        debugUi_ = std::make_unique<DebugUi>(context_, window_, pipeline_->renderPass(),
                                             swapchain_->imageCount(), settings_, *material_,
                                             [this](const std::string& path) { importMesh(path); });
    }

    previousFrameTime_ = window_.elapsedSeconds();
}

Renderer::~Renderer() {
    const VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(device);

    destroySyncObjects();
    freeCommandBuffers();
    destroyFramebuffers();
    destroyDepthResources();

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    // The rest release their own Vulkan objects. They are destroyed after this
    // body, in reverse declaration order, so the mesh goes before the material
    // it borrowed, the descriptor sets before the layout they were allocated
    // from, and the UI before the render pass it draws into.
    //
    // The one thing that must happen first is the wait above: an import may have
    // been recorded into a command buffer that is still executing.
}

void Renderer::run() {
    while (!window_.shouldClose()) {
        window_.pollEvents();

        if (debugUi_) {
            // Built before the frame is recorded so toolbar edits take effect on
            // the frame currently being drawn.
            debugUi_->beginFrame();
            debugUi_->buildUi();
        }

        drawFrame();

        // Checked after the frame so the final UI frame is completed rather than
        // abandoned half-way through.
        if (debugUi_ && debugUi_->wantsQuit()) {
            break;
        }
    }

    vkDeviceWaitIdle(context_.device());
}

void Renderer::advanceRotation() {
    const double now = window_.elapsedSeconds();
    const double delta = now - previousFrameTime_;
    previousFrameTime_ = now;

    // A request from the Reset view button, or an import, is applied here rather
    // than in the UI so that the camera the frame was drawn with is always the
    // one the settings describe.
    if (settings_.resetViewRequested) {
        settings_.resetViewRequested = false;

        // RendererSettings holds the defaults, so there is no second copy of
        // them here to drift out of step.
        const RendererSettings defaults;
        settings_.cameraYaw = defaults.cameraYaw;
        settings_.cameraPitch = defaults.cameraPitch;
        settings_.cameraDistance = defaults.cameraDistance;
    }

    if (!settings_.rotate) {
        return;
    }

    // Wrapping keeps the angle small so float precision does not degrade over a
    // long-running session.
    rotationAngle_ = std::fmod(rotationAngle_ + static_cast<float>(delta) * settings_.rotationSpeed,
                               kTwoPi);
}

void Renderer::updateSceneUniforms(std::uint32_t frame) {
    const VkExtent2D extent = swapchain_->extent();
    const float aspect = extent.height > 0
                             ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                             : 1.0f;

    // Model: spin about the world up axis, centered on the origin, scaled so the
    // bounding sphere has radius one. Folding the centering and the scale in here
    // is what lets the camera always orbit the origin, wherever the file put the
    // mesh and however large it was.
    const math::Mat4 model = math::multiply(
        math::rotationZ(rotationAngle_),
        math::multiply(math::scaling(modelScale_),
                       math::translation(math::Vec3{-modelCenter_[0], -modelCenter_[1],
                                                    -modelCenter_[2]})));

    // Orbit camera: the eye swings around the origin on a sphere of the
    // configured distance, with yaw around the up axis and pitch as elevation.
    const float pitch = std::clamp(settings_.cameraPitch, -kMaxCameraPitch, kMaxCameraPitch);
    const float distance = std::max(settings_.cameraDistance, 0.05f);
    const float cosPitch = std::cos(pitch);
    const math::Vec3 eye{std::cos(settings_.cameraYaw) * cosPitch * distance,
                         std::sin(settings_.cameraYaw) * cosPitch * distance,
                         std::sin(pitch) * distance};

    // Z is up, so that is the reference the camera keeps level.
    const math::Mat4 view =
        math::lookAt(eye, math::Vec3{0.0f, 0.0f, 0.0f}, math::Vec3{0.0f, 0.0f, 1.0f});
    const math::Mat4 projection = math::perspective(kFieldOfView, aspect, kNearPlane, kFarPlane);
    const math::Mat4 viewProjection = math::multiply(projection, view);

    const math::Vec3 light = math::normalize(
        math::Vec3{kLightDirection[0], kLightDirection[1], kLightDirection[2]});

    SceneUniforms uniforms{};
    std::copy_n(model.m, 16, uniforms.model);
    std::copy_n(viewProjection.m, 16, uniforms.viewProjection);
    uniforms.cameraPosition[0] = eye.x;
    uniforms.cameraPosition[1] = eye.y;
    uniforms.cameraPosition[2] = eye.z;
    uniforms.cameraPosition[3] = 1.0f;
    uniforms.lightDirection[0] = light.x;
    uniforms.lightDirection[1] = light.y;
    uniforms.lightDirection[2] = light.z;
    uniforms.lightDirection[3] = 0.0f;
    uniforms.lightColor[0] = kLightColor[0];
    uniforms.lightColor[1] = kLightColor[1];
    uniforms.lightColor[2] = kLightColor[2];
    uniforms.lightColor[3] = 1.0f;

    sceneUniforms_->update(frame, uniforms);
}

void Renderer::importMesh(const std::string& path) {
    try {
        MeshData data = loadMesh(path);
        const std::string name = data.name;
        applyMeshData(std::move(data), path);
        settings_.statusMessage = "Imported '" + name + "'";
        settings_.statusIsError = false;
    } catch (const std::exception& error) {
        // Whatever is on screen stays: a failed import should not leave the
        // renderer with nothing to draw.
        settings_.statusMessage = std::string("Import failed: ") + error.what();
        settings_.statusIsError = true;
    }
}

void Renderer::applyMeshData(MeshData data, const std::string& source) {
    if (data.vertices.empty() || data.indices.empty()) {
        throw std::runtime_error("the mesh has no geometry");
    }

    // Center the mesh and scale it to a bounding sphere of radius one. The
    // camera and the orbit pivot both assume that, so a millimetre and a
    // kilometre model land on screen the same way.
    for (int axis = 0; axis < 3; ++axis) {
        modelCenter_[axis] = (data.boundsMin[axis] + data.boundsMax[axis]) * 0.5f;
    }
    const math::Vec3 diagonal{data.boundsMax[0] - data.boundsMin[0],
                              data.boundsMax[1] - data.boundsMin[1],
                              data.boundsMax[2] - data.boundsMin[2]};
    const float radius = math::length(diagonal) * 0.5f;
    modelScale_ = radius > 1e-6f ? 1.0f / radius : 1.0f;

    // The buffers about to be replaced may still be read by a frame in flight,
    // so the device has to go idle first. Building the new mesh before touching
    // mesh_ also means a throw from the constructor leaves the old one intact.
    vkDeviceWaitIdle(context_.device());
    std::unique_ptr<Mesh> mesh = std::make_unique<Mesh>(context_, std::move(data.vertices),
                                                        std::move(data.indices), *material_);
    mesh_ = std::move(mesh);

    // The shading parameters travel with the file.
    material_->setPbr(data.material);

    settings_.modelName = data.name;
    settings_.modelSource = source;
    settings_.vertexCount = mesh_->vertexCount();
    settings_.indexCount = mesh_->indexCount();

    // Frame the newcomer: the previous camera was aimed at a different shape.
    const RendererSettings cameraDefaults;
    settings_.cameraYaw = cameraDefaults.cameraYaw;
    settings_.cameraPitch = cameraDefaults.cameraPitch;
    settings_.cameraDistance = cameraDefaults.cameraDistance;
}

void Renderer::createCommandPool() {
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = context_.graphicsFamily();

    VK_CHECK(vkCreateCommandPool(context_.device(), &createInfo, nullptr, &commandPool_));
}

void Renderer::createDepthResources() {
    const VkDevice device = context_.device();
    const VkFormat format = pipeline_->depthFormat();
    const VkExtent2D extent = swapchain_->extent();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device, &imageInfo, nullptr, &depthImage_));

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, depthImage_, &requirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        findMemoryType(context_, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device, &allocateInfo, nullptr, &depthMemory_));
    VK_CHECK(vkBindImageMemory(device, depthImage_, depthMemory_, 0));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_DEPTH_BIT |
        (hasStencilComponent(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &depthView_));
}

void Renderer::destroyDepthResources() {
    const VkDevice device = context_.device();

    if (depthView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, depthImage_, nullptr);
        depthImage_ = VK_NULL_HANDLE;
    }
    if (depthMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, depthMemory_, nullptr);
        depthMemory_ = VK_NULL_HANDLE;
    }
}

void Renderer::createFramebuffers() {
    const VkDevice device = context_.device();
    const std::vector<VkImageView>& views = swapchain_->imageViews();
    const VkExtent2D extent = swapchain_->extent();

    framebuffers_.resize(views.size());

    for (std::size_t i = 0; i < views.size(); ++i) {
        // The same depth view is attached to every framebuffer: only one render
        // pass writes into it at a time, and the pass clears it every frame, so
        // there is nothing to keep per swapchain image.
        const VkImageView attachments[] = {views[i], depthView_};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = pipeline_->renderPass();
        createInfo.attachmentCount = 2;
        createInfo.pAttachments = attachments;
        createInfo.width = extent.width;
        createInfo.height = extent.height;
        createInfo.layers = 1;

        VK_CHECK(vkCreateFramebuffer(device, &createInfo, nullptr, &framebuffers_[i]));
    }
}

void Renderer::destroyFramebuffers() {
    for (const VkFramebuffer framebuffer : framebuffers_) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(context_.device(), framebuffer, nullptr);
        }
    }
    framebuffers_.clear();
}

void Renderer::createCommandBuffers() {
    // One primary command buffer per swapchain image: the one used to draw is
    // chosen by the image index returned from vkAcquireNextImageKHR.
    commandBuffers_.resize(swapchain_->imageCount());

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());

    VK_CHECK(vkAllocateCommandBuffers(context_.device(), &allocateInfo, commandBuffers_.data()));
}

void Renderer::freeCommandBuffers() {
    if (!commandBuffers_.empty()) {
        vkFreeCommandBuffers(context_.device(), commandPool_,
                             static_cast<std::uint32_t>(commandBuffers_.size()),
                             commandBuffers_.data());
        commandBuffers_.clear();
    }
}

void Renderer::createSyncObjects() {
    const VkDevice device = context_.device();

    imageAvailable_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);
    // One signal semaphore per swapchain image so a semaphore is never reused
    // while a previous presentation of the same image may still be pending.
    renderFinished_.resize(swapchain_->imageCount());
    imagesInFlight_.assign(swapchain_->imageCount(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // so the first waitFrame() returns at once

    for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable_[i]));
        VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences_[i]));
    }

    for (std::size_t i = 0; i < renderFinished_.size(); ++i) {
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished_[i]));
    }
}

void Renderer::destroySyncObjects() {
    const VkDevice device = context_.device();

    for (const VkSemaphore semaphore : imageAvailable_) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    imageAvailable_.clear();

    for (const VkSemaphore semaphore : renderFinished_) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    renderFinished_.clear();

    for (const VkFence fence : inFlightFences_) {
        vkDestroyFence(device, fence, nullptr);
    }
    inFlightFences_.clear();

    // imagesInFlight_ only aliases fences owned by inFlightFences_.
    imagesInFlight_.clear();
}

void Renderer::recreateSwapchain() {
    // A minimized window reports a zero-sized framebuffer; block until it has a
    // real size again instead of spinning on an invalid swapchain.
    VkExtent2D size = window_.framebufferSize();
    while (size.width == 0 || size.height == 0) {
        glfwWaitEvents();
        size = window_.framebufferSize();
    }

    const VkDevice device = context_.device();
    vkDeviceWaitIdle(device);

    // Framebuffers reference both the swapchain images and the render pass, so
    // they must be released before either is rebuilt. The depth attachment is
    // referenced by the framebuffers too, and its size has to follow the new
    // extent.
    destroyFramebuffers();
    destroyDepthResources();

    swapchain_->recreate(window_);
    pipeline_->recreate(swapchain_->imageFormat());

    if (debugUi_) {
        // The UI pipeline is compiled against the render pass, which
        // GraphicsPipeline::recreate replaces whenever the format changed.
        debugUi_->onSwapchainRecreated(pipeline_->renderPass(), swapchain_->imageCount());
    }

    freeCommandBuffers();
    destroySyncObjects();

    createDepthResources();
    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    // The uniforms for this frame slot are written here, at the last moment
    // before recording: drawFrame() has already waited on the slot's fence, so
    // no frame that could still be reading the previous contents is in flight.
    updateSceneUniforms(currentFrame_);

    // Providing the clear values explicitly means the render pass can use
    // VK_ATTACHMENT_LOAD_OP_CLEAR for both attachments. Depth clears to 1.0, the
    // far plane, so the first fragment written always passes the depth test.
    const VkClearValue clearValues[2] = {
        {{{0.05f, 0.06f, 0.09f, 1.0f}}},
        {{1.0f, 0.0f}},
    };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = pipeline_->renderPass();
    renderPassInfo.framebuffer = framebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchain_->extent();
    renderPassInfo.clearValueCount = 2;
    renderPassInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

    // The scene uniforms come from the descriptor set whose layout the pipeline
    // was built with, so set 0 is the one it expects.
    const VkDescriptorSet sceneSet = sceneUniforms_->descriptorSet(currentFrame_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0, 1,
                            &sceneSet, 0, nullptr);

    // The material rides in a push constant instead of a second uniform buffer:
    // it is a handful of floats, and reading it straight from the material every
    // frame means a value the user just dragged in the UI is in effect for this
    // very frame.
    const PbrMaterial& pbr = material_->pbr();
    MaterialPushConstants materialConstants{};
    std::copy_n(pbr.baseColorFactor, 4, materialConstants.baseColorFactor);
    std::copy_n(pbr.emissiveFactor, 3, materialConstants.emissiveFactor);  // fourth stays 0
    materialConstants.metallicFactor = pbr.metallicFactor;
    materialConstants.roughnessFactor = pbr.roughnessFactor;

    vkCmdPushConstants(commandBuffer, pipeline_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(materialConstants), &materialConstants);

    // Viewport and scissor are dynamic state, so they are set every frame.
    const VkExtent2D extent = swapchain_->extent();

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    // The mesh binds its own vertex and index buffers and issues the indexed
    // draw, so this is all the geometry the pipeline needs to see.
    mesh_->draw(commandBuffer);

    // ImGui shares this render pass, so it has to be recorded before the pass
    // ends. Note it needs no framebuffers of its own - the backend draws into
    // the pass that is already active.
    if (debugUi_) {
        debugUi_->render(commandBuffer);
    }

    vkCmdEndRenderPass(commandBuffer);

    VK_CHECK(vkEndCommandBuffer(commandBuffer));
}

void Renderer::drawFrame() {
    const VkDevice device = context_.device();

    // Wait until the frame slot we are about to reuse is no longer in use.
    VK_CHECK(vkWaitForFences(device, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX));

    std::uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(device, swapchain_->handle(), UINT64_MAX,
                                                         imageAvailable_[currentFrame_],
                                                         VK_NULL_HANDLE, &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        // The window changed size while we were not looking; rebuild and retry
        // on the next iteration.
        recreateSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error(std::string("vkAcquireNextImageKHR failed (") +
                                 vkutil::resultToString(acquireResult) + ")");
    }

    // The image may still be read by a frame that started earlier, so wait for
    // that frame's fence before touching it. This has to happen before the
    // fence of the current slot is reset.
    if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
        VK_CHECK(vkWaitForFences(device, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX));
    }
    imagesInFlight_[imageIndex] = inFlightFences_[currentFrame_];

    VK_CHECK(vkResetFences(device, 1, &inFlightFences_[currentFrame_]));

    recordCommandBuffer(commandBuffers_[imageIndex], imageIndex);

    // Moves the spin on for the next frame by the wall-clock time this one took,
    // and picks up a pending Reset view request.
    advanceRotation();

    const VkSemaphore waitSemaphores[] = {imageAvailable_[currentFrame_]};
    const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signalSemaphores[] = {renderFinished_[imageIndex]};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    VK_CHECK(vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, inFlightFences_[currentFrame_]));

    VkSwapchainKHR swapchains[] = {swapchain_->handle()};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presentResult = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
        window_.consumeResizeFlag()) {
        recreateSwapchain();
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error(std::string("vkQueuePresentKHR failed (") +
                                 vkutil::resultToString(presentResult) + ")");
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}
