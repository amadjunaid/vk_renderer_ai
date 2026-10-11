#include "vk/DebugUi.hpp"

#include "core/Window.hpp"
#include "vk/VulkanContext.hpp"
#include "vk/VkCheck.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

/// ImGui insists on at least IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE
/// (8) sampled-image descriptors when it creates its own pool.
constexpr std::uint32_t kDescriptorPoolSize = 64;

/// ImGui asserts MinImageCount >= 2 and ImageCount >= MinImageCount, which a
/// swapchain is normally well above - but a surface may cap maxImageCount at 1.
std::uint32_t clampImageCount(std::uint32_t swapchainImageCount) {
    return std::max(2u, swapchainImageCount);
}

ImGui_ImplVulkan_PipelineInfo makePipelineInfo(VkRenderPass renderPass) {
    ImGui_ImplVulkan_PipelineInfo info{};
    info.RenderPass = renderPass;
    info.Subpass = 0;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    return info;
}

void checkVkResult(VkResult result) {
    if (result != VK_SUCCESS) {
        std::cerr << "[imgui] Vulkan call failed: " << vkutil::resultToString(result) << '\n';
    }
}

}  // namespace

DebugUi::DebugUi(const VulkanContext& context, const Window& window, VkRenderPass renderPass,
                 std::uint32_t swapchainImageCount, RendererSettings& settings)
    : context_(context), settings_(settings), renderPass_(renderPass) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Keep the working tree clean: with no ini file ImGui starts from defaults
    // every run. Assign a path (e.g. ImGui::GetIO().IniFilename) to persist
    // window positions instead.
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForVulkan(window.handle(), true)) {
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui_ImplGlfw_InitForVulkan failed");
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = context_.apiVersion();
    initInfo.Instance = context_.instance();
    initInfo.PhysicalDevice = context_.physicalDevice();
    initInfo.Device = context_.device();
    initInfo.QueueFamily = context_.graphicsFamily();
    initInfo.Queue = context_.graphicsQueue();

    // DescriptorPool and DescriptorPoolSize are mutually exclusive; leaving the
    // pool null lets the backend create and own one.
    initInfo.DescriptorPoolSize = kDescriptorPoolSize;

    const std::uint32_t imageCount = clampImageCount(swapchainImageCount);
    initInfo.MinImageCount = imageCount;
    initInfo.ImageCount = imageCount;

    // Rendering through a classic render pass, which the UI shares with the
    // mesh, instead of the dynamic rendering path.
    initInfo.PipelineInfoMain = makePipelineInfo(renderPass_);
    initInfo.UseDynamicRendering = false;
    initInfo.PipelineCache = VK_NULL_HANDLE;

    // A floor of 1 MiB keeps the best-practices validation layer from warning
    // about the backend's small vertex/index buffer allocations.
    initInfo.MinAllocationSize = 1024 * 1024;
    initInfo.CheckVkResultFn = &checkVkResult;

    // No font atlas is created here. ImGui 1.92 uploads textures lazily through
    // the renderer texture system (ImGuiBackendFlags_RendererHasTextures), so
    // the ImGui_ImplVulkan_CreateFontsTexture() call of older releases is gone.
    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    }
}

DebugUi::~DebugUi() {
    if (context_.device() == VK_NULL_HANDLE) {
        return;
    }

    // The backend owns GPU resources, so nothing may still be in flight.
    vkDeviceWaitIdle(context_.device());

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void DebugUi::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void DebugUi::buildUi() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Quit", "Esc")) {
                quitRequested_ = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Toolbar", nullptr, &showToolbar_);
            ImGui::MenuItem("Dear ImGui Demo", nullptr, &showDemoWindow_);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("About", nullptr, &showAboutWindow_);
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    if (showToolbar_) {
        drawToolbar();
    }
    if (showDemoWindow_) {
        // The demo is the fastest way to browse what ImGui can draw.
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
    if (showAboutWindow_) {
        drawAboutWindow();
    }
}

void DebugUi::drawToolbar() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;

    // WorkPos/WorkSize already exclude the main menu bar, so the toolbar sits
    // directly beneath it.
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##toolbar", nullptr, flags)) {
        if (ImGui::Button(settings_.rotate ? "Pause" : "Play")) {
            settings_.rotate = !settings_.rotate;
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset speed")) {
            settings_.rotationSpeed = RendererSettings{}.rotationSpeed;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Speed (rad/s)", &settings_.rotationSpeed, 0.0f, 4.0f, "%.2f");

        ImGui::SameLine();
        ImGui::TextDisabled("|");

        ImGui::SameLine();
        ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DebugUi::drawAboutWindow() {
    if (ImGui::Begin("About", &showAboutWindow_)) {
        ImGui::Text("deepseek_vk_renderer");
        ImGui::Separator();
        ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
        ImGui::TextWrapped(
            "The UI shares the mesh's render pass, so it is recorded into the same "
            "command buffer.");
    }
    ImGui::End();
}

void DebugUi::render(VkCommandBuffer commandBuffer) const {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

void DebugUi::onSwapchainRecreated(VkRenderPass renderPass, std::uint32_t swapchainImageCount) {
    ImGui_ImplVulkan_SetMinImageCount(clampImageCount(swapchainImageCount));

    if (renderPass == renderPass_) {
        return;
    }

    // The pipeline was compiled against the previous render pass, which has now
    // been destroyed, so rebuild it against the new one. Creating just the
    // pipeline avoids a full backend shutdown, which would throw away the
    // ImGui context and every texture with it.
    renderPass_ = renderPass;
    const ImGui_ImplVulkan_PipelineInfo info = makePipelineInfo(renderPass_);
    ImGui_ImplVulkan_CreateMainPipeline(&info);
}
