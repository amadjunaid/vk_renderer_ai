#include "vk/Buffer.hpp"

#include "vk/VkCheck.hpp"
#include "vk/VulkanContext.hpp"

#include <cstring>
#include <stdexcept>

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

Buffer createHostVisibleBuffer(const VulkanContext& context, const void* data, VkDeviceSize size,
                               VkBufferUsageFlags usage) {
    const Buffer buffer =
        createBuffer(context, size, usage,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(context.device(), buffer.memory, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(context.device(), buffer.memory);

    return buffer;
}
