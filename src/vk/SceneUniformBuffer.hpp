#pragma once

#include "vk/Buffer.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class VulkanContext;

/// Everything the shaders read once per frame.
///
/// The layout has to match the `SceneUniforms` block in shaders/mesh.vert and
/// shaders/mesh.frag: std140 lines every mat4 and vec4 up on a 16-byte
/// boundary, which a struct of float[16] and float[4] members already does - so
/// this can be copied into the buffer as-is, exactly like Vertex is uploaded.
///
/// `modelViewProjection` is kept as one matrix rather than three because the
/// vertex shader only ever needs the product, and `model` separately so the
/// shader can build the normal matrix.
struct SceneUniforms {
    float model[16];
    float viewProjection[16];
    float cameraPosition[4];  // xyz: the eye, for the specular term
    float lightDirection[4];  // xyz: normalized, pointing from the surface to the light
    float lightColor[4];      // rgb: light color premultiplied by its intensity
};

/// The per-frame uniform buffer and the descriptor set that points at it.
///
/// One buffer holds one slice per frame in flight and every slice has its own
/// descriptor set, so a frame the GPU is still reading is never overwritten by
/// the frame the CPU is preparing. The slices are strided by
/// VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment because a descriptor
/// may not point at an arbitrary offset into a uniform buffer.
class SceneUniformBuffer {
public:
    SceneUniformBuffer(const VulkanContext& context, VkDescriptorSetLayout setLayout,
                       std::uint32_t framesInFlight);
    ~SceneUniformBuffer();

    SceneUniformBuffer(const SceneUniformBuffer&) = delete;
    SceneUniformBuffer& operator=(const SceneUniformBuffer&) = delete;

    /// Overwrites the slice belonging to `frame`. The caller must have waited on
    /// that frame slot's fence first.
    void update(std::uint32_t frame, const SceneUniforms& uniforms);

    /// The set to bind at set 0 for the given frame.
    VkDescriptorSet descriptorSet(std::uint32_t frame) const { return descriptorSets_[frame]; }

private:
    const VulkanContext& context_;

    std::uint32_t framesInFlight_ = 0;

    /// Bytes between the start of one frame's slice and the next.
    VkDeviceSize stride_ = 0;

    Buffer buffer_;
    void* mapped_ = nullptr;  // the whole allocation, kept mapped for the run

    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;
};
