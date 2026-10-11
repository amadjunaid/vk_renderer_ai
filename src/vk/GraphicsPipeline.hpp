#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

class VulkanContext;
class Material;

/// Push constant block shared with shaders/triangle.vert.
///
/// The renderer fills this in every frame with vkCmdPushConstants. It must stay
/// byte-compatible with the `PushConstants` block in the shader: floats need no
/// padding, but adding a vec2/vec4 (or reordering fields) means matching the
/// std430 rules the shader compiler applies.
struct TrianglePushConstants {
    float angle = 0.0f;  // rotation around the origin, in radians
};

/// The render pass and the single graphics pipeline that draws the mesh.
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

private:
    void createRenderPass();
    void createPipeline();
    void destroy();

    static VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code);
    static std::vector<char> readFile(const std::string& path);

    const VulkanContext& context_;
    const Material& material_;

    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
