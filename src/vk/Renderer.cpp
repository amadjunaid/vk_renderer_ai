#include "vk/Renderer.hpp"

#include "core/Window.hpp"
#include "vk/GraphicsPipeline.hpp"
#include "vk/Swapchain.hpp"
#include "vk/VulkanContext.hpp"
#include "vk/VkCheck.hpp"

#include <stdexcept>

Renderer::Renderer(const VulkanContext& context, const Window& window, const std::string& shaderDir)
    : context_(context), window_(window), shaderDir_(shaderDir) {
    swapchain_ = std::make_unique<Swapchain>(context_, window_);
    pipeline_ = std::make_unique<GraphicsPipeline>(context_, swapchain_->imageFormat(), shaderDir_);

    createCommandPool();
    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();
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

    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    // pipeline_ and swapchain_ release their own Vulkan objects.
}

void Renderer::run() {
    while (!window_.shouldClose()) {
        window_.pollEvents();
        drawFrame();
    }

    vkDeviceWaitIdle(context_.device());
}

void Renderer::createCommandPool() {
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = context_.graphicsFamily();

    VK_CHECK(vkCreateCommandPool(context_.device(), &createInfo, nullptr, &commandPool_));
}

void Renderer::createFramebuffers() {
    const VkDevice device = context_.device();
    const std::vector<VkImageView>& views = swapchain_->imageViews();
    const VkExtent2D extent = swapchain_->extent();

    framebuffers_.resize(views.size());

    for (std::size_t i = 0; i < views.size(); ++i) {
        const VkImageView attachments[] = {views[i]};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = pipeline_->renderPass();
        createInfo.attachmentCount = 1;
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
    // they must be released before either is rebuilt.
    destroyFramebuffers();

    swapchain_->recreate(window_);
    pipeline_->recreate(swapchain_->imageFormat(), shaderDir_);

    freeCommandBuffers();
    destroySyncObjects();

    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();
}

void Renderer::recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    // Providing the clear color explicitly means the render pass can use
    // VK_ATTACHMENT_LOAD_OP_CLEAR.
    VkClearValue clearColor{};
    clearColor.color = {{0.05f, 0.06f, 0.09f, 1.0f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = pipeline_->renderPass();
    renderPassInfo.framebuffer = framebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchain_->extent();
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

    // The vertex shader builds the rotation matrix itself; the CPU only supplies
    // the angle, which advances with wall-clock time.
    const TrianglePushConstants pushConstants{
        static_cast<float>(window_.elapsedSeconds()) * kRotationSpeed};
    vkCmdPushConstants(commandBuffer, pipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(pushConstants), &pushConstants);

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

    // Three vertices, no vertex buffer, no indices.
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

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
