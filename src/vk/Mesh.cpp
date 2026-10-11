#include "vk/Mesh.hpp"

#include "vk/VkCheck.hpp"
#include "vk/VulkanContext.hpp"

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace {

/// A buffer and the memory backing it. They are always created and destroyed
/// together, so they travel as one value.
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

std::uint32_t findMemoryType(const VulkanContext& context, std::uint32_t typeFilter,
                             VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &memoryProperties);

    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool typeAllowed = (typeFilter & (1u << i)) != 0;
        const bool hasProperties =
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeAllowed && hasProperties) {
            return i;
        }
    }

    throw std::runtime_error("No Vulkan memory type satisfies the requested properties");
}

Buffer createBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties) {
    const VkDevice device = context.device();

    Buffer buffer;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // only the graphics queue touches it
    VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer.handle));

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(context, requirements.memoryTypeBits, properties);
    VK_CHECK(vkAllocateMemory(device, &allocateInfo, nullptr, &buffer.memory));

    VK_CHECK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));

    return buffer;
}

void destroyBuffer(const VulkanContext& context, Buffer& buffer) {
    const VkDevice device = context.device();

    if (buffer.handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer.handle, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buffer.memory, nullptr);
    }

    buffer = {};
}

/// Copies `size` bytes between two buffers on the graphics queue and makes the
/// write visible to the vertex input stage. The command pool is created and
/// thrown away per call: buffers are only ever filled at load time, so a pool
/// kept around for uploads would be dead weight the rest of the run.
void copyBuffer(const VulkanContext& context, VkBuffer source, VkBuffer destination,
                VkDeviceSize size, VkAccessFlags destinationAccess) {
    const VkDevice device = context.device();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = context.graphicsFamily();

    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = pool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, source, destination, 1, &copyRegion);

    // The copy is a transfer-stage write; without the barrier the vertex fetch
    // could read the destination before any of it landed.
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = destinationAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = destination;
    barrier.offset = 0;
    barrier.size = size;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);

    VK_CHECK(vkEndCommandBuffer(commandBuffer));

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    // Nothing else uses the graphics queue while a mesh is being built, so the
    // submission needs no semaphore and waiting on the queue is enough.
    VK_CHECK(vkQueueSubmit(context.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(context.graphicsQueue()));

    vkDestroyCommandPool(device, pool, nullptr);
}

/// Fills a buffer in device-local memory. That memory is not host visible on a
/// discrete GPU, so the bytes go through a staging buffer.
Buffer createDeviceLocalBuffer(const VulkanContext& context, const void* data, VkDeviceSize size,
                               VkBufferUsageFlags usage) {
    const VkDevice device = context.device();

    Buffer staging = createBuffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device, staging.memory, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device, staging.memory);

    Buffer buffer = createBuffer(context, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    const VkAccessFlags readAccess = (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0
                                         ? VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                                         : VK_ACCESS_INDEX_READ_BIT;
    copyBuffer(context, staging.handle, buffer.handle, size, readAccess);

    destroyBuffer(context, staging);

    return buffer;
}

}  // namespace

VkVertexInputBindingDescription Vertex::bindingDescription() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

std::array<VkVertexInputAttributeDescription, 2> Vertex::attributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 2> attributes{};

    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);

    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, color);

    return attributes;
}

Mesh::Mesh(const VulkanContext& context, std::vector<Vertex> vertices,
           std::vector<std::uint32_t> indices, const Material& material)
    : context_(context), material_(material) {
    if (vertices.empty() || indices.empty()) {
        throw std::runtime_error("A mesh needs at least one vertex and one index");
    }

    vertexCount_ = static_cast<std::uint32_t>(vertices.size());
    indexCount_ = static_cast<std::uint32_t>(indices.size());

    const Buffer vertexBuffer = createDeviceLocalBuffer(
        context_, vertices.data(), vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    vertexBuffer_ = vertexBuffer.handle;
    vertexMemory_ = vertexBuffer.memory;

    const Buffer indexBuffer = createDeviceLocalBuffer(
        context_, indices.data(), indices.size() * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    indexBuffer_ = indexBuffer.handle;
    indexMemory_ = indexBuffer.memory;
}

Mesh::Mesh(Mesh&& other) noexcept
    : context_(other.context_),
      material_(other.material_),
      vertexBuffer_(other.vertexBuffer_),
      vertexMemory_(other.vertexMemory_),
      indexBuffer_(other.indexBuffer_),
      indexMemory_(other.indexMemory_),
      vertexCount_(other.vertexCount_),
      indexCount_(other.indexCount_) {
    // The moved-from mesh must not release what this one now owns.
    other.vertexBuffer_ = VK_NULL_HANDLE;
    other.vertexMemory_ = VK_NULL_HANDLE;
    other.indexBuffer_ = VK_NULL_HANDLE;
    other.indexMemory_ = VK_NULL_HANDLE;
    other.vertexCount_ = 0;
    other.indexCount_ = 0;
}

Mesh::~Mesh() {
    const VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) {
        return;
    }

    if (vertexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vertexBuffer_, nullptr);
    }
    if (vertexMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, vertexMemory_, nullptr);
    }
    if (indexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, indexBuffer_, nullptr);
    }
    if (indexMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, indexMemory_, nullptr);
    }
}

void Mesh::draw(VkCommandBuffer commandBuffer) const {
    const VkBuffer vertexBuffers[] = {vertexBuffer_};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, indexCount_, 1, 0, 0, 0);
}
