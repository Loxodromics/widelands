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

#ifndef WL_GRAPHIC_RHI_VULKAN_VULKAN_COMMAND_BUFFER_H
#define WL_GRAPHIC_RHI_VULKAN_VULKAN_COMMAND_BUFFER_H

#ifdef WL_BUILD_VULKAN

#include <memory>
#include <set>
#include <string>

#include <volk.h>

#include "graphic/rhi/rhi.h"

namespace Rhi {

class VulkanPipeline;
class VulkanPipelineCache;

// The Vulkan command buffer (WP-15): records the RenderQueue's draw calls
// into a VkCommandBuffer. The pass state (render pass, framebuffer, extent)
// comes from a Target so the recording logic is target-agnostic - the screen
// variant carries the acquired swapchain framebuffer, the offscreen variant
// (the headless test, WP-16b) a VulkanTexture.
//
// Viewport and scissor are dynamic pipeline state (WP-14). The canonical
// coordinates (RHI_INTERFACE.md §2.4: clip Y up, window origin bottom-left)
// are compensated here: the committed SPIR-V negates gl_Position.y (WP-13),
// so the viewport must be flipped too - the canonical bottom-left rect
// (x, y, w, h) maps to the top-origin rect (x, H - y - h) with positive
// height - and the scissor gets the same flip. The depth range is 0..1; the
// z remap lives in the SPIR-V wrapper main.
//
// WP-15 draw-skip rule: a pipeline whose descriptor set layout declares
// bindings cannot draw yet (descriptor sets are WP-16), so draws of those
// pipelines are dropped with a one-time warning per program. This keeps the
// validation layers silent while the game runs to completion.
class VulkanCommandBuffer : public CommandBuffer {
public:
	// A render target to record into.
	struct Target {
		VkRenderPass render_pass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VkExtent2D extent{};
		// Number of clear values the render pass expects (2 for the screen
		// pass with depth, 1 for a color-only offscreen pass).
		uint32_t clear_value_count = 0;
	};

	// 'cache' may be null when every bound pipeline carries a direct handle
	// (the headless test); the screen path always passes the real cache.
	VulkanCommandBuffer(VkCommandBuffer command_buffer, const VulkanPipelineCache* cache,
	                    Target screen_target);
	~VulkanCommandBuffer() override;

	void begin_pass(const Texture* target, const PassClear& clear) override;
	void set_viewport(const Recti& viewport) override;
	void set_scissor(const Recti& rect) override;
	void disable_scissor() override;
	void bind_pipeline(const Pipeline* pipeline) override;
	void bind_descriptor_set(const DescriptorSet* set) override;
	void bind_vertex_buffer(const Buffer* buffer) override;
	void draw(uint32_t vertex_offset, uint32_t vertex_count) override;
	void transition(const Texture* texture, TextureLayout layout) override;
	void end_pass() override;

	// Whether vkCmdBeginRenderPass was recorded and not yet ended.
	bool pass_open() const;

	// Ends a still-open pass and ends the underlying VkCommandBuffer. Called
	// by the device's end_frame before submitting; a no-op once finished.
	void finish();

private:
	void record_scissor(const Recti& rect);

	VkCommandBuffer command_buffer_;
	const VulkanPipelineCache* cache_;

	Target screen_target_;
	VkExtent2D current_extent_{};

	const VulkanPipeline* current_pipeline_ = nullptr;

	bool pass_open_ = false;
	bool finished_ = false;

	DISALLOW_COPY_AND_ASSIGN(VulkanCommandBuffer);
};

// The command buffer for paths Vulkan does not implement yet (WP-15): the
// immediate render-to-texture path (texture.cc's do_* methods, WP-16b) and
// frames dropped by a swapchain recreation. Every method is a no-op so the
// game keeps running - textures rendered into this way stay blank, which is
// invisible while nothing samples textures (no descriptor binding until
// WP-16).
class VulkanNoOpCommandBuffer : public CommandBuffer {
public:
	VulkanNoOpCommandBuffer() = default;

	void begin_pass(const Texture* target, const PassClear& clear) override;
	void set_viewport(const Recti& viewport) override;
	void set_scissor(const Recti& rect) override;
	void disable_scissor() override;
	void bind_pipeline(const Pipeline* pipeline) override;
	void bind_descriptor_set(const DescriptorSet* set) override;
	void bind_vertex_buffer(const Buffer* buffer) override;
	void draw(uint32_t vertex_offset, uint32_t vertex_count) override;
	void transition(const Texture* texture, TextureLayout layout) override;
	void end_pass() override;

	DISALLOW_COPY_AND_ASSIGN(VulkanNoOpCommandBuffer);
};

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_COMMAND_BUFFER_H
