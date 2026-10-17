#include "vk/SceneUniformBuffer.hpp"

#include "vk/VkCheck.hpp"
#include "vk/VulkanContext.hpp"

#include <cstring>
#include <vector>

SceneUniformBuffer::SceneUniformBuffer(const VulkanContext& context, VkDescriptorSetLayout setLayout,
                                       std::uint32_t framesInFlight)
    : context_(context), framesInFlight_(framesInFlight) {
    const VkDevice device = context_.device();

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice(), &properties);
    const VkDeviceSize alignment = properties.limits.minUniformBufferOffsetAlignment > 0
                                       ? properties.limits.minUniformBufferOffsetAlignment
                                       : 1;
    const VkDeviceSize elementSize = sizeof(SceneUniforms);
    stride_ = ((elementSize + alignment - 1) / alignment) * alignment;

    const VkDeviceSize allocationSize = stride_ * framesInFlight_;
    buffer_ = createBuffer(context_, allocationSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // Mapped once and never unmapped: two small writes per frame are not worth
    // a map/unmap pair each time.
    VK_CHECK(vkMapMemory(device, buffer_.memory, 0, allocationSize, 0, &mapped_));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = framesInFlight_;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = framesInFlight_;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_));

    const std::vector<VkDescriptorSetLayout> layouts(framesInFlight_, setLayout);

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool_;
    allocateInfo.descriptorSetCount = framesInFlight_;
    allocateInfo.pSetLayouts = layouts.data();

    descriptorSets_.resize(framesInFlight_);
    VK_CHECK(vkAllocateDescriptorSets(device, &allocateInfo, descriptorSets_.data()));

    for (std::uint32_t frame = 0; frame < framesInFlight_; ++frame) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = buffer_.handle;
        bufferInfo.offset = frame * stride_;
        bufferInfo.range = elementSize;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSets_[frame];
        write.dstBinding = 0;  // matches `layout(set = 0, binding = 0)` in the shaders
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

SceneUniformBuffer::~SceneUniformBuffer() {
    const VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // The descriptor sets belong to the pool, so the pool releases them.
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }

    if (mapped_ != nullptr) {
        vkUnmapMemory(device, buffer_.memory);
        mapped_ = nullptr;
    }
    destroyBuffer(context_, buffer_);
    descriptorSets_.clear();
}

void SceneUniformBuffer::update(std::uint32_t frame, const SceneUniforms& uniforms) {
    if (frame >= framesInFlight_ || mapped_ == nullptr) {
        return;
    }

    // Host-coherent memory, so there is no flush to do after the copy.
    std::memcpy(static_cast<char*>(mapped_) + frame * stride_, &uniforms, sizeof(SceneUniforms));
}
