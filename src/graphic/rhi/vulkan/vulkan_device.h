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

#ifndef WL_GRAPHIC_RHI_VULKAN_VULKAN_DEVICE_H
#define WL_GRAPHIC_RHI_VULKAN_VULKAN_DEVICE_H

#include <memory>

#include "graphic/rhi/rhi.h"

struct SDL_Window;

// The Vulkan implementation of the RHI (renderer modernization plan, WP-12
// bootstrap, WP-14 pipelines/render pass, WP-15 conversion): an instance, a
// physical/logical device, a surface and a swapchain wired to the SDL
// window, the screen render pass with its depth attachment and framebuffers,
// the twelve pre-built pipelines, and - since WP-15 - the frame loop
// (begin_frame/end_frame) and the recording path (VulkanCommandBuffer) plus
// the per-frame staging arena (VulkanBuffer). WP-16 adds descriptor sets
// (per-frame descriptor pool, binding via the manifest), texture creation
// and upload (VulkanTexture + the one-shot upload command pool), and the
// samplers the descriptor writes reference. volk (src/third_party/volk) is
// the loader.
//
// Registered with Rhi::set_device in the constructor, so the eight programs
// route their draws here from the first frame on. What is deliberately
// missing until the WPs named below, all implemented as loud or quiet stubs
// rather than crashes:
//   - immediate render-to-texture (WP-16b): begin_offscreen returns a no-op
//     command buffer,
//   - swapchain readback (WP-18): read_back_swapchain throws.
// The hidden GL window (graphic.cc) stays under Vulkan solely so
// Texture::lock()'s glReadPixels readback keeps returning its (blank, until
// WP-16b) data; texture creation and upload went to Vulkan in WP-16.
//
// All Vulkan types stay out of this header (pimpl); only the .cc includes
// volk and the Vulkan headers.
namespace Rhi {

class VulkanDevice : public Device {
public:
	// Creates the instance, device, surface and swapchain for 'window' (which
	// must have been created with the SDL_WINDOW_VULKAN flag), and registers
	// this object as the active RHI device. Throws wexception on any failure,
	// so the game fails with a clear error rather than half-initializing.
	explicit VulkanDevice(SDL_Window* window);
	~VulkanDevice() override;

	Backend backend() const override;

	// The physical device's maxImageDimension2D - the Vulkan-side replacement
	// for the GL_MAX_TEXTURE_SIZE the atlas builder has sized itself against
	// until now (WP-16 moves texture creation to Vulkan).
	[[nodiscard]] uint32_t max_texture_size() const;

	std::unique_ptr<CommandBuffer> begin_frame() override;
	void end_frame(std::unique_ptr<CommandBuffer> command_buffer) override;
	std::unique_ptr<CommandBuffer> begin_offscreen() override;
	void submit_offscreen(std::unique_ptr<CommandBuffer> command_buffer) override;
	void read_back_swapchain(uint8_t* pixels) override;
	std::unique_ptr<Texture> create_texture(const TextureDescriptor& desc) override;
	std::unique_ptr<Texture> create_texture_view(Texture& parent, const Recti& subrect) override;
	std::unique_ptr<Buffer> create_buffer(uint32_t size, BufferUsage usage) override;
	std::unique_ptr<Pipeline> create_pipeline(const PipelineDescriptor& desc) override;
	std::unique_ptr<DescriptorSet> create_descriptor_set(const Pipeline& pipeline) override;

	DISALLOW_COPY_AND_ASSIGN(VulkanDevice);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_DEVICE_H
