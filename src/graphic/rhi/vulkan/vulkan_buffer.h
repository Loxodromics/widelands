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

#ifndef WL_GRAPHIC_RHI_VULKAN_VULKAN_BUFFER_H
#define WL_GRAPHIC_RHI_VULKAN_VULKAN_BUFFER_H

#ifdef WL_BUILD_VULKAN

#include <mutex>
#include <vector>

#include <volk.h>

#include "graphic/rhi/rhi.h"

namespace Rhi {

// The staging arena behind VulkanBuffer (renderer modernization plan, WP-15;
// RHI_INTERFACE.md §2.6 transient-resource semantics): one large
// HOST_VISIBLE | HOST_COHERENT VkBuffer (grown by adding more when a frame
// outgrows it) with a linear bump offset. Every allocate() hands out a fresh
// region, so a buffer may be updated and drawn repeatedly within one command
// buffer and each recorded draw reads the region that was current when it
// was recorded - exactly how the eight programs use it today. reset()
// rewinds the offset to zero.
//
// Since WP-17 there is one arena per frame slot (frames in flight), so the
// device owns an array of arenas and a "current arena" pointer that switches
// to the next slot in begin_frame. The owner (VulkanDevice) calls reset() on
// a slot's arena at the start of that slot's next use, after the slot's
// submit fence has been waited, so no live region is ever reused.
//
// Threading: allocations can arrive from the initializer thread (offscreen
// render-to-texture updates between frames), so the bump offset is guarded
// by a mutex. reset() runs on the UI thread in begin_frame, when no other
// thread is mid-update (offscreen submits fence-wait before returning).
class VulkanArena {
public:
	// One allocation: the buffer plus the byte offset of 'size' contiguous,
	// alignment-aligned bytes, and the host pointer to write them through.
	struct Region {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		VkDeviceSize size = 0;
		void* mapped = nullptr;
	};

	// 'memory_type_index' is the host-visible coherent memory type of
	// 'device', chosen once by the caller. 'initial_size' is the size of the
	// first buffer; further buffers are allocated on demand.
	VulkanArena(VkDevice device, uint32_t memory_type_index, uint32_t alignment,
	            VkDeviceSize initial_size);
	~VulkanArena();

	// Allocates a fresh region of at least 'size' bytes; copies nothing.
	// Throws wexception when the device is out of memory.
	Region allocate(uint32_t size);

	// Rewinds all allocations. Only valid once everything recorded against
	// them has completed (the fence wait in the frame loop).
	void reset();

private:
	// One arena chunk: its VkBuffer, the device memory backing it, and the
	// persistent host mapping allocations write through.
	struct ArenaChunk {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;
		VkDeviceSize size = 0;
	};

	VkDevice device_;
	uint32_t memory_type_index_;
	uint32_t alignment_;

	std::vector<ArenaChunk> chunks_;
	VkDeviceSize capacity_ = 0;
	VkDeviceSize offset_ = 0;
	std::mutex mutex_;

	DISALLOW_COPY_AND_ASSIGN(VulkanArena);
};

// A vertex or uniform buffer whose storage comes from the device's per-frame
// arena. The 'size' argument of create_buffer is an initial capacity hint;
// arena-backed buffers ignore it (rhi.h permits this: the hint only avoids a
// first re-allocation, which arena regions never pay). Every update()
// allocates a fresh region, so the transient-resource contract holds.
//
// Buffers are long-lived while arenas rotate per frame slot (WP-17), so the
// buffer holds a reference to the device's *current arena pointer* rather
// than to one arena: update() always allocates from whichever slot's arena is
// active at that moment. The device re-points the pointer in begin_frame;
// between frames it still names the last begun frame's arena, which is safe
// because offscreen work queued there is retired before that slot resets.
class VulkanBuffer : public Buffer {
public:
	explicit VulkanBuffer(VulkanArena*& arena);

	void update(const void* data, uint32_t size) override;

	// The arena region the most recent update() allocated - what a recorded
	// draw must bind. The buffer handle is valid even before the first
	// update (the first arena buffer exists), but the offset/size then
	// describe no written data; callers update before drawing.
	VkBuffer buffer() const;
	VkDeviceSize offset() const;

private:
	VulkanArena*& arena_;
	VulkanArena::Region region_;

	DISALLOW_COPY_AND_ASSIGN(VulkanBuffer);
};

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_BUFFER_H
