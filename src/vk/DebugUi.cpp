#include "vk/DebugUi.hpp"

#include "core/FileDialog.hpp"
#include "core/Window.hpp"
#include "vk/VulkanContext.hpp"
#include "vk/VkCheck.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {

/// ImGui insists on at least IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE
/// (8) sampled-image descriptors when it creates its own pool.
constexpr std::uint32_t kDescriptorPoolSize = 64;

/// Status colours for the last import result.
const ImVec4 kOkColor{0.55f, 0.85f, 0.55f, 1.0f};
const ImVec4 kErrorColor{1.0f, 0.5f, 0.4f, 1.0f};

/// How the viewport is navigated, spelled out for the platform: a Mac trackpad
/// has no middle button, so the emulation the renderer accepts for it is the
/// one worth advertising there.
#if defined(__APPLE__)
constexpr const char* kNavigationHint = "Option+drag orbit, +Shift pan, +Cmd or wheel zoom";
#else
constexpr const char* kNavigationHint = "MMB orbit, Shift+MMB pan, Ctrl+MMB or wheel zoom";
#endif

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
                 std::uint32_t swapchainImageCount, RendererSettings& settings, Material& material,
                 ImportCallback onImportMesh)
    : context_(context),
      settings_(settings),
      material_(material),
      onImportMesh_(std::move(onImportMesh)),
      nativeWindow_(window.nativeHandle()),
      renderPass_(renderPass) {
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
            if (ImGui::BeginMenu("Import")) {
                drawImportMenu();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                quitRequested_ = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Toolbar", nullptr, &showToolbar_);
            ImGui::MenuItem("Model", nullptr, &showModelWindow_);
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
    if (showModelWindow_) {
        drawModelWindow();
    }
    if (showDemoWindow_) {
        // The demo is the fastest way to browse what ImGui can draw.
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
    if (showAboutWindow_) {
        drawAboutWindow();
    }
}

void DebugUi::drawImportMenu() {
    const bool hasDialog = filedialog::available();

    if (ImGui::MenuItem("Mesh...", "glTF .glb, FBX .fbx")) {
        if (hasDialog) {
            const std::optional<std::string> path =
                filedialog::openMeshFile("Import mesh", nativeWindow_);
            // Nothing to do on cancel: an empty optional means the user closed
            // the dialog, which is not an error worth reporting.
            if (path.has_value() && onImportMesh_) {
                onImportMesh_(*path);
            }
        } else if (onImportMesh_ && importPath_[0] != '\0') {
            // No native dialog on this platform: use the path from the model
            // window instead of doing nothing, so the menu item still works.
            onImportMesh_(importPath_);
        }
    }

    ImGui::Separator();
    if (hasDialog) {
        ImGui::TextDisabled("Supported: .glb, .fbx");
    } else {
        ImGui::TextDisabled("Type a .glb or .fbx path into the Model window");
    }
}

void DebugUi::drawModelWindow() {
    // A fixed starting width keeps the sliders from being squeezed into the
    // default auto-fit size on the first frame.
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Model", &showModelWindow_)) {
        ImGui::SeparatorText("Mesh");
        ImGui::Text("Name: %s", settings_.modelName.empty() ? "-" : settings_.modelName.c_str());
        if (!settings_.modelSource.empty()) {
            ImGui::TextDisabled("Source: %s", settings_.modelSource.c_str());
        }
        ImGui::Text("%u vertices, %u indices (%u triangles)", settings_.vertexCount,
                    settings_.indexCount, settings_.indexCount / 3);

        if (!filedialog::available()) {
            // Fallback for platforms without the native dialog: the path field
            // the Import menu reads.
            ImGui::SetNextItemWidth(-90.0f);
            ImGui::InputText("##meshPath", importPath_, sizeof(importPath_));
            ImGui::SameLine();
            if (ImGui::Button("Import") && importPath_[0] != '\0') {
                if (onImportMesh_) {
                    onImportMesh_(importPath_);
                }
            }
        }

        if (!settings_.statusMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  settings_.statusIsError ? kErrorColor : kOkColor);
            ImGui::TextWrapped("%s", settings_.statusMessage.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::SeparatorText("Material");
        ImGui::TextDisabled("%s", material_.pbr().name.c_str());

        // Edited in place: these are the exact values the renderer pushes to the
        // fragment shader for the next frame.
        PbrMaterial& pbr = material_.pbr();
        ImGui::ColorEdit4("Base color", pbr.baseColorFactor, ImGuiColorEditFlags_Float);
        ImGui::SliderFloat("Metallic", &pbr.metallicFactor, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Roughness", &pbr.roughnessFactor, 0.0f, 1.0f, "%.3f");
        ImGui::ColorEdit3("Emissive", pbr.emissiveFactor, ImGuiColorEditFlags_Float);

        bool doubleSided = material_.pbr().doubleSided;
        if (ImGui::Checkbox("Double sided", &doubleSided)) {
            material_.pbr().doubleSided = doubleSided;
        }
        ImGui::TextDisabled("informational: both faces are always drawn");

        ImGui::SeparatorText("Camera");
        ImGui::TextWrapped("viewport: %s", kNavigationHint);
        ImGui::TextDisabled("world: +Z up, -X forward");
        ImGui::SliderFloat("Yaw", &settings_.camera.yaw, -3.1416f, 3.1416f, "%.2f rad");
        ImGui::SliderFloat("Pitch", &settings_.camera.pitch, OrbitCamera::kMinPitch,
                           OrbitCamera::kMaxPitch, "%.2f rad");
        ImGui::SliderFloat("Distance", &settings_.camera.distance, OrbitCamera::kMinDistance,
                           OrbitCamera::kMaxDistance, "%.2f");
        float pivot[3] = {settings_.camera.target.x, settings_.camera.target.y,
                          settings_.camera.target.z};
        if (ImGui::DragFloat3("Pivot", pivot, 0.01f)) {
            settings_.camera.target = math::Vec3{pivot[0], pivot[1], pivot[2]};
        }
        if (ImGui::Button("Reset view")) {
            settings_.resetViewRequested = true;
        }
    }
    ImGui::End();
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
        if (ImGui::Button("Reset view")) {
            settings_.resetViewRequested = true;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");

        ImGui::SameLine();
        ImGui::TextDisabled("%s", kNavigationHint);

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
        ImGui::TextWrapped(
            "File > Import > Mesh loads a binary glTF (.glb) or an FBX (.fbx) file: its "
            "vertices, its normals and its material's PBR factors.");
    }
    ImGui::End();
}

bool DebugUi::wantsMouseCapture() const {
    // Only meaningful after beginFrame(), which the renderer calls before it
    // updates the camera, so the answer matches the frame being built.
    return ImGui::GetIO().WantCaptureMouse;
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
