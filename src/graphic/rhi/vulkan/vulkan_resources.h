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

#ifndef WL_GRAPHIC_RHI_VULKAN_VULKAN_RESOURCES_H
#define WL_GRAPHIC_RHI_VULKAN_VULKAN_RESOURCES_H

#ifdef WL_BUILD_VULKAN

#include <memory>
#include <string>
#include <unordered_map>

#include <volk.h>

#include "graphic/rhi/rhi.h"

namespace Rhi {

// The Vulkan pipeline wrapper (WP-15). Either resolves its VkPipeline through
// the pipeline cache at bind time (the twelve screen pipelines, built by
// WP-14), or carries a direct handle for pipelines the cache does not know -
// the offscreen render targets of WP-16b, and the headless test's target.
class VulkanPipeline : public Pipeline {
public:
	// Cache-resolved: 'program_name' + 'blend' look the handle up at bind
	// time. 'requires_binding' says whether the program's descriptor set
	// layout declares any bindings (drives the WP-15 draw-skip rule).
	VulkanPipeline(std::string program_name, BlendState blend, bool requires_binding);

	// Direct handle: used as-is; not owned (the owner destroys it after the
	// pipeline object). 'handle' must not be null.
	VulkanPipeline(std::string program_name, BlendState blend, bool requires_binding,
	               VkPipeline handle);

	const std::string& program_name() const;
	const BlendState& blend() const;
	bool requires_binding() const;
	// VK_NULL_HANDLE means "resolve through the pipeline cache".
	VkPipeline handle() const;

private:
	std::string program_name_;
	BlendState blend_;
	bool requires_binding_;
	VkPipeline handle_;

	DISALLOW_COPY_AND_ASSIGN(VulkanPipeline);
};

// The Vulkan descriptor set (WP-15 stub, real in WP-16): stores the bindings
// the programs record, so WP-16 only has to allocate and bind them. Nothing
// binds in WP-15 - the command buffer skips draws of pipelines whose layout
// has bindings (see VulkanCommandBuffer::draw).
class VulkanDescriptorSet : public DescriptorSet {
public:
	explicit VulkanDescriptorSet(bool requires_binding);

	void set_texture(uint32_t binding, const Texture* texture) override;
	void set_uniform_buffer(uint32_t binding,
	                        const Buffer* buffer,
	                        uint32_t offset,
	                        uint32_t size) override;

	bool requires_binding() const;

	// The recorded bindings (consumed by WP-16).
	const std::unordered_map<uint32_t, const Texture*>& textures() const;
	struct UniformBinding {
		const Buffer* buffer = nullptr;
		uint32_t offset = 0;
		uint32_t size = 0;
	};
	const std::unordered_map<uint32_t, UniformBinding>& uniform_buffers() const;

private:
	bool requires_binding_;
	std::unordered_map<uint32_t, const Texture*> textures_;
	std::unordered_map<uint32_t, UniformBinding> uniform_buffers_;

	DISALLOW_COPY_AND_ASSIGN(VulkanDescriptorSet);
};

// A Vulkan image usable as a render target and (from WP-16 on) as a sampled
// texture. In WP-15 only the headless test constructs one, wrapping an
// already-created image/view/render pass/framebuffer; WP-16 (create_texture)
// and WP-16b (offscreen targets) build on it. Owns all five handles.
class VulkanTexture : public Texture {
public:
	VulkanTexture(VkDevice device,
	              uint32_t width,
	              uint32_t height,
	              VkImage image,
	              VkDeviceMemory image_memory,
	              VkImageView view,
	              VkRenderPass render_pass,
	              VkFramebuffer framebuffer);
	~VulkanTexture() override;

	uint32_t width() const override;
	uint32_t height() const override;
	void upload(const void* pixels) override;
	void read_back(uint8_t* pixels) override;

	VkImage image() const;
	VkImageView view() const;
	VkRenderPass render_pass() const;
	VkFramebuffer framebuffer() const;

private:
	VkDevice device_;
	uint32_t width_;
	uint32_t height_;
	VkImage image_;
	VkDeviceMemory image_memory_;
	VkImageView view_;
	VkRenderPass render_pass_;
	VkFramebuffer framebuffer_;

	DISALLOW_COPY_AND_ASSIGN(VulkanTexture);
};

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_RESOURCES_H
