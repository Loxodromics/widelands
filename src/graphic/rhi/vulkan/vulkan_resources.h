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
#include <mutex>
#include <string>
#include <unordered_map>

#include <volk.h>

#include "graphic/rhi/rhi.h"
#include "graphic/rhi/vulkan/vulkan_manifest.h"

namespace Rhi {

// Everything VulkanTexture::upload needs beyond its own image, shared with
// the device so textures do not each carry a queue or command pool. The
// context outlives the textures (it is owned by VulkanDevice::Impl), and all
// uploads go through one fence so two initializer-thread uploads cannot
// interleave on the one-shot command buffer.
struct VulkanUploadContext {
	VkDevice device = VK_NULL_HANDLE;
	// The physical device, for picking device-local and host-visible memory
	// types when a texture's image memory or the staging buffer is allocated.
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	// A pool dedicated to one-shot upload command buffers (kept separate from
	// the frame's command pool so an upload never competes with the frame's
	// in-flight recording).
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	// The upload staging buffer: a persistent, grow-on-demand host-visible
	// buffer reused by every upload (overwritten after the fence wait). It is
	// deliberately NOT the per-frame arena: startup uploads happen outside
	// the frame loop, so arena regions would accumulate without a reset -
	// and on the NVIDIA box the arena prefers the 246 MB BAR heap, which a
	// full startup's worth of never-recycled staging would exhaust (WP-16).
	VkBuffer staging_buffer = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory = VK_NULL_HANDLE;
	void* staging_mapped = nullptr;
	VkDeviceSize staging_size = 0;

	// Serializes uploads (they share the staging buffer, the one-shot command
	// pool and the fence); the texture images themselves are per-upload.
	std::mutex mutex;
};

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
// the programs record in the RHI's per-type binding indices (textures index
// the samplers, uniform buffers are always index 0 - each program has at
// most one block). It also carries the pipeline-specific pieces the command
// buffer needs at bind time: the manifest entry (binding translation), the
// descriptor set layout (allocation) and the pipeline layout
// (vkCmdBindDescriptorSets). The command buffer allocates the VkDescriptorSet
// from the device's per-frame pool, writes the descriptors and binds it.
class VulkanDescriptorSet : public DescriptorSet {
public:
	VulkanDescriptorSet(std::string program_name,
	                    const ManifestProgram* manifest,
	                    VkDescriptorSetLayout set_layout,
	                    VkPipelineLayout pipeline_layout,
	                    bool requires_binding);

	void set_texture(uint32_t binding, const Texture* texture) override;
	void set_uniform_buffer(uint32_t binding,
	                        const Buffer* buffer,
	                        uint32_t offset,
	                        uint32_t size) override;

	const std::string& program_name() const;
	const ManifestProgram* manifest() const;
	VkDescriptorSetLayout set_layout() const;
	VkPipelineLayout pipeline_layout() const;
	bool requires_binding() const;

	// The recorded bindings (consumed by the command buffer at bind time).
	const std::unordered_map<uint32_t, const Texture*>& textures() const;
	struct UniformBinding {
		const Buffer* buffer = nullptr;
		uint32_t offset = 0;
		uint32_t size = 0;
	};
	const std::unordered_map<uint32_t, UniformBinding>& uniform_buffers() const;

private:
	std::string program_name_;
	const ManifestProgram* manifest_;
	VkDescriptorSetLayout set_layout_;
	VkPipelineLayout pipeline_layout_;
	bool requires_binding_;
	std::unordered_map<uint32_t, const Texture*> textures_;
	std::unordered_map<uint32_t, UniformBinding> uniform_buffers_;

	DISALLOW_COPY_AND_ASSIGN(VulkanDescriptorSet);
};

// A Vulkan image usable as a render target (WP-16b) and, since WP-16, as a
// sampled texture with a real upload path. Two construction shapes:
//   - the sampled texture create_texture produces: the class owns the image,
//     its memory and its view, uploads through the VulkanUploadContext, and
//     carries the device's sampler for its filter. Since WP-16b every RGBA8
//     texture is *also* a potential render target: the image is created with
//     colour-attachment usage and 'render_pass' is the device's shared
//     colour-only offscreen render pass (not owned), against which the
//     framebuffer is built lazily on first use (ensure_framebuffer).
//   - the offscreen target the headless test builds: wraps already-created
//     image/view/render pass/framebuffer handles, owning all of them.
// The render pass and framebuffer are VK_NULL_HANDLE until the texture is
// first drawn into (a texture that is never a render target never builds a
// framebuffer).
class VulkanTexture : public Texture {
public:
	// The sampled-texture shape (create_texture): creates the image, memory,
	// view and sampler for 'format'/'filter' and uploads through 'upload'.
	// 'render_pass' is the device's shared offscreen render pass for render
	// targets (VK_NULL_HANDLE in headless tests that never render into the
	// texture); the texture does not own it.
	VulkanTexture(VkDevice device,
	              uint32_t width,
	              uint32_t height,
	              TextureFormat format,
	              TextureFilter filter,
	              VkSampler sampler,
	              VulkanUploadContext* upload,
	              VkRenderPass render_pass = VK_NULL_HANDLE);
	// The offscreen-target shape (the headless test): wraps the given
	// handles, owns all of them. 'sampler' is null until the target is also
	// sampled.
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
	VkSampler sampler() const;
	VkRenderPass render_pass() const;
	VkFramebuffer framebuffer() const;
	// The image layout the texture is currently in (WP-16: upload leaves it
	// shader-read-only; WP-16b builds its transitions on this).
	VkImageLayout current_layout() const;

	// Creates the framebuffer against the shared render pass, if the texture
	// has not been a render target before. WP-16b: command buffers call this
	// from begin_pass, so a texture that is never drawn into never allocates
	// one. Throws when the texture was created without a render pass.
	void ensure_framebuffer();

	// WP-16b: the render pass's final layout after end_pass, recorded by the
	// command buffer so later transitions start from the real layout.
	void set_current_layout(VkImageLayout layout);

private:
	VkDevice device_;
	uint32_t width_;
	uint32_t height_;
	VkFormat format_;
	VkImage image_;
	VkDeviceMemory image_memory_;
	VkImageView view_;
	VkSampler sampler_;
	VkRenderPass render_pass_;
	VkFramebuffer framebuffer_;
	// Whether this texture owns render_pass_ (the offscreen-target shape) or
	// borrows the device's shared pass (the sampled-texture shape).
	bool owns_render_pass_;
	VulkanUploadContext* upload_;
	VkImageLayout current_layout_;

	DISALLOW_COPY_AND_ASSIGN(VulkanTexture);
};

// The pipeline stage and access mask a layout transition needs on one side,
// used by both VulkanTexture::upload and VulkanCommandBuffer::transition
// (WP-16b). UNDEFINED has no source stage/access; TRANSFER_DST covers the
// upload copy.
struct LayoutTransitionInfo {
	VkPipelineStageFlags stage;
	VkAccessFlags access;
};
LayoutTransitionInfo layout_transition_source(VkImageLayout layout);
LayoutTransitionInfo layout_transition_destination(VkImageLayout layout);

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_RESOURCES_H
