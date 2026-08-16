/*
 * Copyright (C) 2026 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "graphic/rhi/vulkan/vulkan_buffer.h"

#ifdef WL_BUILD_VULKAN

#include <algorithm>
#include <cstring>

#include "base/wexception.h"

namespace Rhi {

// One arena buffer: its VkBuffer, the device memory backing it, and the
// persistent host mapping arena allocations write through.
struct ArenaChunk {
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkDeviceSize size = 0;
};

VulkanArena::VulkanArena(const VkDevice device,
                         const uint32_t memory_type_index,
                         const uint32_t alignment,
                         const VkDeviceSize initial_size)
   : device_(device),
     memory_type_index_(memory_type_index),
     alignment_(std::max(alignment, 16u)) {
	// Allocate the first chunk eagerly so a buffer created with a zero
	// capacity hint still has a valid VkBuffer to bind from day one.
	static_cast<void>(
	   allocate(static_cast<uint32_t>(std::max<VkDeviceSize>(initial_size, 1u << 20))));
}

VulkanArena::~VulkanArena() {
	for (const ArenaChunk& chunk : chunks_) {
		if (chunk.mapped != nullptr) {
			vkUnmapMemory(device_, chunk.memory);
		}
		if (chunk.memory != VK_NULL_HANDLE) {
			vkFreeMemory(device_, chunk.memory, nullptr);
		}
		if (chunk.buffer != VK_NULL_HANDLE) {
			vkDestroyBuffer(device_, chunk.buffer, nullptr);
		}
	}
}

VulkanArena::Region VulkanArena::allocate(const uint32_t size) {
	std::lock_guard<std::mutex> lock(mutex_);
	Region region{};
	if (size == 0u) {
		// A zero-size probe: the first chunk's buffer, once the arena has
		// one. The constructor's own first allocation takes the growth path
		// below, so only callers after construction see a buffer here.
		if (!chunks_.empty()) {
			region.buffer = chunks_.front().buffer;
		}
		return region;
	}

	// Round up so the next region starts aligned; chunk boundaries stay
	// aligned too, since the offset only ever enters a fresh chunk at its
	// start.
	const VkDeviceSize aligned_size = (static_cast<VkDeviceSize>(size) + alignment_ - 1) /
	                                  alignment_ * alignment_;

	// Find the chunk containing offset_ (a region must lie entirely inside
	// one chunk - each chunk is a separate VkBuffer allocation, and a
	// recorded draw binds (buffer, offset), so a region spanning two chunks
	// would not be addressable).
	VkDeviceSize chunk_start = 0;
	size_t chunk_index = chunks_.size();
	for (size_t i = 0; i < chunks_.size(); ++i) {
		const VkDeviceSize chunk_end = chunk_start + chunks_[i].size;
		if (offset_ >= chunk_start && offset_ < chunk_end) {
			chunk_index = i;
			break;
		}
		chunk_start = chunk_end;
	}

	// Grow a fresh chunk when offset_ sits at the very end of the arena or
	// the region does not fit into its chunk. The previous chunks stay
	// referenced by already-recorded draws.
	if (chunk_index == chunks_.size() ||
	    offset_ + aligned_size > chunk_start + chunks_[chunk_index].size) {
		const VkDeviceSize chunk_size =
		   std::max<VkDeviceSize>(aligned_size, chunks_.empty() ? 0 : chunks_.back().size);
		VkBufferCreateInfo buffer_create_info{};
		buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_create_info.size = chunk_size;
		buffer_create_info.usage =
		   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ArenaChunk chunk{};
		chunk.size = chunk_size;
		if (vkCreateBuffer(device_, &buffer_create_info, nullptr, &chunk.buffer) != VK_SUCCESS) {
			throw wexception("Vulkan: vkCreateBuffer failed for the staging arena");
		}

		VkMemoryRequirements memory_requirements{};
		vkGetBufferMemoryRequirements(device_, chunk.buffer, &memory_requirements);
		VkMemoryAllocateInfo allocate_info{};
		allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize = memory_requirements.size;
		allocate_info.memoryTypeIndex = memory_type_index_;
		if (vkAllocateMemory(device_, &allocate_info, nullptr, &chunk.memory) != VK_SUCCESS) {
			throw wexception("Vulkan: vkAllocateMemory failed for the staging arena");
		}
		if (vkBindBufferMemory(device_, chunk.buffer, chunk.memory, 0) != VK_SUCCESS) {
			throw wexception("Vulkan: vkBindBufferMemory failed for the staging arena");
		}
		if (vkMapMemory(device_, chunk.memory, 0, chunk_size, 0, &chunk.mapped) != VK_SUCCESS) {
			throw wexception("Vulkan: vkMapMemory failed for the staging arena");
		}
		chunks_.push_back(chunk);
		capacity_ += chunk_size;
		// The fresh chunk starts where the arena ended; the unused tail of
		// the previous chunk is left behind.
		offset_ = capacity_ - chunk_size;
		chunk_start = offset_;
		chunk_index = chunks_.size() - 1;
	}

	region.buffer = chunks_[chunk_index].buffer;
	// The offset a recorded draw binds is relative to the region's VkBuffer
	// (the chunk), not to the arena as a whole.
	region.offset = offset_ - chunk_start;
	region.size = size;
	region.mapped = static_cast<char*>(chunks_[chunk_index].mapped) + region.offset;
	offset_ += aligned_size;
	return region;
}

void VulkanArena::reset() {
	std::lock_guard<std::mutex> lock(mutex_);
	offset_ = 0;
}

VulkanBuffer::VulkanBuffer(VulkanArena& arena) : arena_(arena) {
	// Touch the arena so the buffer has a bindable VkBuffer immediately; the
	// actual per-frame region is allocated by update().
	region_ = arena_.allocate(0);
}

void VulkanBuffer::update(const void* data, const uint32_t size) {
	region_ = arena_.allocate(size);
	if (size > 0u) {
		std::memcpy(region_.mapped, data, size);
	}
}

VkBuffer VulkanBuffer::buffer() const {
	return region_.buffer;
}

VkDeviceSize VulkanBuffer::offset() const {
	return region_.offset;
}

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
