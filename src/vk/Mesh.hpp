#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

class Material;
class VulkanContext;

/// One vertex as the mesh stores it and as the vertex shader reads it: a 2D
/// position and an RGB color. This is the vertex format of every mesh in the
/// renderer, so the layout here, the descriptions below and the `inPosition` /
/// `inColor` inputs in shaders/triangle.vert have to agree.
struct Vertex {
    float position[2];
    float color[3];

    /// The binding and attribute descriptions GraphicsPipeline declares for
    /// this vertex format.
    static VkVertexInputBindingDescription bindingDescription();
    static std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions();
};

/// A triangle mesh: the vertices and indices that describe a surface, the
/// device-local buffers they are uploaded to, and the material it is drawn with.
///
/// The mesh only borrows the material, so the owner has to keep it alive for as
/// long as the mesh is drawn. The buffers are built once, at construction, and
/// never written again: a static mesh needs no upload path.
class Mesh {
public:
    Mesh(const VulkanContext& context, std::vector<Vertex> vertices,
         std::vector<std::uint32_t> indices, const Material& material);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    /// Takes over the other mesh's GPU buffers, which leaves it owning nothing.
    /// The context and material references are copied, not moved.
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&&) = delete;

    /// Binds the vertex and index buffers and draws the whole mesh. The caller
    /// must have bound a pipeline whose vertex layout matches Vertex.
    void draw(VkCommandBuffer commandBuffer) const;

    const Material& material() const { return material_; }

    std::uint32_t vertexCount() const { return vertexCount_; }
    std::uint32_t indexCount() const { return indexCount_; }

private:
    const VulkanContext& context_;
    const Material& material_;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory_ = VK_NULL_HANDLE;

    std::uint32_t vertexCount_ = 0;
    std::uint32_t indexCount_ = 0;
};
