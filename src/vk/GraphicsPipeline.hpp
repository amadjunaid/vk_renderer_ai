#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

class VulkanContext;
class Material;

/// Per-material constants, pushed before every draw.
///
/// The renderer fills this in every frame with vkCmdPushConstants. It must stay
/// byte-compatible with the `MaterialPushConstants` block in shaders/mesh.frag:
/// std430 lines every vec4 up on a 16-byte boundary, so the emissive factor is
/// padded to four floats here even though the shader only reads three. Adding a
/// vec2/vec4 or reordering the fields means re-checking that padding.
struct MaterialPushConstants {
    float baseColorFactor[4];
    float emissiveFactor[4];
    float metallicFactor;
    float roughnessFactor;
};

/// The render pass and the single graphics pipeline that draws the mesh.
///
/// The pass has two attachments: the swapchain image and a depth buffer, which
/// is what lets a 3D mesh draw its own far side before its near side. The depth
/// format is chosen once by the device query below and is then fixed for the
/// life of the pipeline, so the renderer can build the matching image from it.
///
/// Viewport and scissor are declared as dynamic state, so resizing the window
/// does not require rebuilding the pipeline - only a change of the swapchain
/// image format does.
///
/// The pipeline is built from one material and keeps only a reference to it, so
/// the material has to outlive the pipeline. The vertex layout comes from Vertex
/// (see vk/Mesh.hpp), which the pipeline declares for the mesh that it draws.
class GraphicsPipeline {
public:
    GraphicsPipeline(const VulkanContext& context, VkFormat colorFormat, const Material& material);
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    /// Rebuilds the pipeline for a new swapchain color format. A no-op when the
    /// format did not change.
    void recreate(VkFormat colorFormat);

    VkRenderPass renderPass() const { return renderPass_; }
    VkPipelineLayout layout() const { return layout_; }
    VkPipeline handle() const { return pipeline_; }

    /// The layout of the scene-wide uniform buffer, bound at set 0 before each
    /// draw. Renderer builds its SceneUniformBuffer from this.
    VkDescriptorSetLayout descriptorSetLayout() const { return descriptorSetLayout_; }

    /// The depth attachment format the render pass was built for. Renderer
    /// creates the depth image and view from it, so both ends agree.
    VkFormat depthFormat() const { return depthFormat_; }

private:
    /// Picks the device's preferred depth format among the ones every Vulkan
    /// implementation is expected to support for rendering. Fixed at
    /// construction: a mid-run change would mean rebuilding the pass and the
    /// pipeline for a size change, and no device needs that.
    static VkFormat chooseDepthFormat(const VulkanContext& context);

    void createRenderPass();
    void createDescriptorSetLayout();
    void createPipeline();
    void destroy();

    static VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
    static std::vector<char> readFile(const std::string& path);

    const VulkanContext& context_;
    const Material& material_;

    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
