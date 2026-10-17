#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

class VulkanContext;

/// A VkBuffer and the memory backing it.
///
/// They are always created and destroyed together, so they travel as one value
/// instead of as two members that could drift apart. Shared by the mesh buffers
/// and the per-frame uniform buffer.
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

/// Index of the first memory type that both `typeFilter` allows and satisfies
/// `properties`. Throws when nothing matches, which means the request cannot be
/// satisfied on this device at all.
std::uint32_t findMemoryType(const VulkanContext& context, std::uint32_t typeFilter,
                             VkMemoryPropertyFlags properties);

/// Creates a buffer of `size` bytes and the memory bound to it. `size` must be
/// greater than zero: a zero-sized VkBuffer is not a valid object.
Buffer createBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties);

/// Destroys both handles and resets the value to a null one, so the pair can be
/// released twice by accident without leaking or double-freeing.
void destroyBuffer(const VulkanContext& context, Buffer& buffer);

/// Creates a host-visible, host-coherent buffer and writes `data` into it.
/// This is for small amounts of data the CPU touches repeatedly; anything read
/// by the GPU in bulk belongs in device-local memory (see Mesh's staging path).
Buffer createHostVisibleBuffer(const VulkanContext& context, const void* data, VkDeviceSize size,
                               VkBufferUsageFlags usage);
