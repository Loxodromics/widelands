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

#include "graphic/rhi/vulkan/vulkan_command_buffer.h"

#ifdef WL_BUILD_VULKAN

#include <algorithm>
#include <vector>

#include "base/wexception.h"
#include "graphic/rhi/vulkan/vulkan_buffer.h"
#include "graphic/rhi/vulkan/vulkan_manifest.h"
#include "graphic/rhi/vulkan/vulkan_pipeline_cache.h"
#include "graphic/rhi/vulkan/vulkan_resources.h"

namespace Rhi {

namespace {

// The canonical -> Vulkan window-space flip for scissor rectangles (the
// plain rect flip; the clip-space half of the compensation is the shader's
// Y-negation plus the flipped viewport rect). Intersects the rect with the
// framebuffer first, so the scissor VUIDs (non-negative offset, offset +
// extent within the framebuffer) hold even for partially off-screen
// rectangles; an empty intersection becomes a zero-size scissor.
Recti flip_rect(const Recti& rect, const VkExtent2D extent) {
	const int framebuffer_width = static_cast<int>(extent.width);
	const int framebuffer_height = static_cast<int>(extent.height);
	const int x0 = std::max(rect.x, 0);
	const int y0 = std::max(rect.y, 0);
	const int x1 = std::min(rect.x + rect.w, framebuffer_width);
	const int y1 = std::min(rect.y + rect.h, framebuffer_height);
	if (x1 <= x0 || y1 <= y0) {
		return Recti(0, 0, 0, 0);
	}
	return Recti(x0, framebuffer_height - y1, x1 - x0, y1 - y0);
}

}  // namespace

VulkanCommandBuffer::VulkanCommandBuffer(const VkDevice device,
                                         const VkCommandBuffer command_buffer,
                                         const VulkanPipelineCache* cache,
                                         Target screen_target,
                                         const VkDescriptorPool descriptor_pool,
                                         const VulkanTexture* dummy_texture)
   : device_(device),
     command_buffer_(command_buffer),
     cache_(cache),
     descriptor_pool_(descriptor_pool),
     dummy_texture_(dummy_texture),
     screen_target_(screen_target) {
}

VulkanCommandBuffer::~VulkanCommandBuffer() = default;

void VulkanCommandBuffer::begin_pass(const Texture* target, const PassClear& clear) {
	if (pass_open_) {
		throw wexception("Vulkan: begin_pass inside an open render pass");
	}
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkExtent2D extent{};
	uint32_t clear_value_count = 0;
	if (target == nullptr) {
		render_pass = screen_target_.render_pass;
		framebuffer = screen_target_.framebuffer;
		extent = screen_target_.extent;
		clear_value_count = screen_target_.clear_value_count;
	} else {
		const VulkanTexture* texture = static_cast<const VulkanTexture*>(target);
		render_pass = texture->render_pass();
		framebuffer = texture->framebuffer();
		extent = {texture->width(), texture->height()};
		clear_value_count = 1;
	}
	if (render_pass == VK_NULL_HANDLE || framebuffer == VK_NULL_HANDLE) {
		throw wexception("Vulkan: begin_pass without a valid render target");
	}

	// The screen render pass clears colour and depth via loadOp; depth always
	// clears to 1.0 = far (RHI_INTERFACE.md §2.5). The clear colour comes from
	// the caller's PassClear.
	VkClearValue clear_values[2] {};
	clear_values[0].color = {{clear.r, clear.g, clear.b, clear.a}};
	clear_values[1].depthStencil = {1.0f, 0};

	VkRenderPassBeginInfo render_pass_begin_info{};
	render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass_begin_info.renderPass = render_pass;
	render_pass_begin_info.framebuffer = framebuffer;
	render_pass_begin_info.renderArea = {{0, 0}, extent};
	render_pass_begin_info.clearValueCount = clear_value_count;
	render_pass_begin_info.pClearValues = clear_values;
	vkCmdBeginRenderPass(command_buffer_, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

	current_extent_ = extent;
	pass_open_ = true;

	// A new pass starts with the scissor "disabled" (covering the whole
	// framebuffer), matching the GL scissor-test default.
	record_scissor(Recti(0, 0, static_cast<int>(extent.width), static_cast<int>(extent.height)));
}

void VulkanCommandBuffer::set_viewport(const Recti& viewport) {
	// The viewport compensates the committed SPIR-V Y-flip (WP-13): Vulkan's
	// window origin is top-left and its clip-space y points down, so a
	// canonical bottom-left rect (x, y, w, h) maps to the top-origin rect
	// (x, H - y - h) with a *positive* height. The clip-space half of the
	// compensation (the y negation) happens in the shader wrapper; flipping
	// the rect here completes it. Depth is 0..1 - the z remap to [0, 1]
	// happens in the shader wrapper too.
	const float framebuffer_height = static_cast<float>(current_extent_.height);
	const VkViewport vk_viewport{static_cast<float>(viewport.x),
	                             framebuffer_height - static_cast<float>(viewport.y) -
	                                static_cast<float>(viewport.h),
	                             static_cast<float>(viewport.w),
	                             static_cast<float>(viewport.h),
	                             0.0f,
	                             1.0f};
	vkCmdSetViewport(command_buffer_, 0, 1, &vk_viewport);
}

void VulkanCommandBuffer::set_scissor(const Recti& rect) {
	record_scissor(rect);
}

void VulkanCommandBuffer::disable_scissor() {
	record_scissor(
	   Recti(0, 0, static_cast<int>(current_extent_.width), static_cast<int>(current_extent_.height)));
}

void VulkanCommandBuffer::bind_pipeline(const Pipeline* pipeline) {
	const VulkanPipeline* vulkan_pipeline = static_cast<const VulkanPipeline*>(pipeline);
	VkPipeline handle = vulkan_pipeline->handle();
	if (handle == VK_NULL_HANDLE) {
		if (cache_ == nullptr) {
			throw wexception("Vulkan: cache-resolved pipeline '%s' with no pipeline cache",
			                 vulkan_pipeline->program_name().c_str());
		}
		handle = cache_->pipeline(vulkan_pipeline->program_name(), vulkan_pipeline->blend());
	}
	vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, handle);
	current_pipeline_ = vulkan_pipeline;
}

void VulkanCommandBuffer::bind_descriptor_set(const DescriptorSet* set) {
	if (current_pipeline_ == nullptr) {
		throw wexception("Rhi::CommandBuffer::bind_descriptor_set: no pipeline bound.");
	}
	if (set == nullptr) {
		throw wexception("Rhi::CommandBuffer::bind_descriptor_set: null descriptor set.");
	}
	if (descriptor_pool_ == VK_NULL_HANDLE) {
		throw wexception("Rhi::CommandBuffer::bind_descriptor_set: no descriptor pool.");
	}
	const VulkanDescriptorSet* vulkan_set = static_cast<const VulkanDescriptorSet*>(set);

	// The manifest is the binding translation: the RHI records per-type
	// indices (texture i = the i-th sampler, uniform buffer 0 = the block),
	// the Vulkan shaders were compiled with the manifest's one shared
	// counter. The set carries its own manifest entry and layouts, so the
	// command buffer needs no cache lookup here.
	const ManifestProgram* manifest = vulkan_set->manifest();
	if (manifest == nullptr) {
		throw wexception("Vulkan: descriptor set of program '%s' has no bindings manifest entry",
		                 vulkan_set->program_name().c_str());
	}

	VkDescriptorSetAllocateInfo allocate_info{};
	allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate_info.descriptorPool = descriptor_pool_;
	allocate_info.descriptorSetCount = 1;
	const VkDescriptorSetLayout set_layout = vulkan_set->set_layout();
	allocate_info.pSetLayouts = &set_layout;
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	if (vkAllocateDescriptorSets(device_, &allocate_info, &descriptor_set) != VK_SUCCESS) {
		throw wexception("Vulkan: vkAllocateDescriptorSets failed for program '%s'",
		                 vulkan_set->program_name().c_str());
	}

	// Translate and write the recorded bindings.
	std::vector<VkWriteDescriptorSet> writes;
	std::vector<VkDescriptorImageInfo> image_infos;
	std::vector<VkDescriptorBufferInfo> buffer_infos;
	image_infos.reserve(vulkan_set->textures().size() + 1);
	buffer_infos.reserve(vulkan_set->uniform_buffers().size());

	for (const auto& [texture_index, texture] : vulkan_set->textures()) {
		if (texture_index >= manifest->samplers.size()) {
			throw wexception(
			   "Vulkan: program '%s' binds texture index %u but declares only %" PRIuS
			   " sampler(s) (the inert grid/workarea binding is gone in WP-16)",
			   vulkan_set->program_name().c_str(), texture_index, manifest->samplers.size());
		}
		const VulkanTexture* vulkan_texture = static_cast<const VulkanTexture*>(texture);
		if (vulkan_texture == nullptr) {
			// A null texture binding (the blit program's absent mask): the
			// shader must not dereference an invalid view, so substitute the
			// device's 1x1 white dummy.
			vulkan_texture = dummy_texture_;
		}
		if (vulkan_texture == nullptr) {
			throw wexception("Vulkan: null texture binding without a dummy texture");
		}
		VkDescriptorImageInfo image_info{};
		image_info.sampler = vulkan_texture->sampler();
		image_info.imageView = vulkan_texture->view();
		image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_infos.push_back(image_info);

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = descriptor_set;
		write.dstBinding = manifest->samplers[texture_index].second;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &image_infos.back();
		writes.push_back(write);
	}

	for (const auto& [buffer_index, binding] : vulkan_set->uniform_buffers()) {
		if (buffer_index >= manifest->uniform_blocks.size()) {
			throw wexception("Vulkan: program '%s' binds uniform buffer index %u but declares "
			                 "only %" PRIuS " block(s)",
			                 vulkan_set->program_name().c_str(), buffer_index,
			                 manifest->uniform_blocks.size());
		}
		const VulkanBuffer* vulkan_buffer = static_cast<const VulkanBuffer*>(binding.buffer);
		if (vulkan_buffer == nullptr) {
			throw wexception("Vulkan: null uniform buffer in a descriptor set of program '%s'",
			                 vulkan_set->program_name().c_str());
		}
		VkDescriptorBufferInfo buffer_info{};
		buffer_info.buffer = vulkan_buffer->buffer();
		buffer_info.offset = vulkan_buffer->offset() + binding.offset;
		buffer_info.range = binding.size;
		buffer_infos.push_back(buffer_info);

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = descriptor_set;
		write.dstBinding = manifest->uniform_blocks[buffer_index].binding;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &buffer_infos.back();
		writes.push_back(write);
	}

	if (!writes.empty()) {
		vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0,
		                       nullptr);
	}

	vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        vulkan_set->pipeline_layout(), 0, 1, &descriptor_set, 0, nullptr);
	current_descriptor_set_ = vulkan_set;
}

void VulkanCommandBuffer::bind_vertex_buffer(const Buffer* buffer) {
	const VulkanBuffer* vulkan_buffer = static_cast<const VulkanBuffer*>(buffer);
	const VkBuffer handle = vulkan_buffer->buffer();
	const VkDeviceSize offset = vulkan_buffer->offset();
	vkCmdBindVertexBuffers(command_buffer_, 0, 1, &handle, &offset);
}

void VulkanCommandBuffer::draw(const uint32_t vertex_offset, const uint32_t vertex_count) {
	if (current_pipeline_ == nullptr) {
		throw wexception("Rhi::CommandBuffer::draw: no pipeline bound.");
	}
	// A pipeline whose layout declares bindings needs a descriptor set bound
	// before drawing (the WP-15 skip rule is gone in WP-16): without one the
	// draw would read undefined samplers/uniforms and trip the validation
	// layers, so fail loudly instead.
	if (current_pipeline_->requires_binding() && current_descriptor_set_ == nullptr) {
		throw wexception("Rhi::CommandBuffer::draw: program '%s' requires a bound descriptor set.",
		                 current_pipeline_->program_name().c_str());
	}
	vkCmdDraw(command_buffer_, vertex_count, 1, vertex_offset, 0);
}

void VulkanCommandBuffer::transition(const Texture* /* texture */, const TextureLayout /* layout */) {
	// Image-layout barriers are WP-16b's; the screen render pass owns its
	// attachment transitions, and nothing renders into a VulkanTexture yet.
}

void VulkanCommandBuffer::end_pass() {
	if (!pass_open_) {
		throw wexception("Vulkan: end_pass without an open render pass");
	}
	vkCmdEndRenderPass(command_buffer_);
	pass_open_ = false;
}

bool VulkanCommandBuffer::pass_open() const {
	return pass_open_;
}

void VulkanCommandBuffer::finish() {
	if (finished_) {
		return;
	}
	if (pass_open_) {
		vkCmdEndRenderPass(command_buffer_);
		pass_open_ = false;
	}
	vkEndCommandBuffer(command_buffer_);
	finished_ = true;
}

void VulkanCommandBuffer::record_scissor(const Recti& rect) {
	const Recti flipped = flip_rect(rect, current_extent_);
	const VkRect2D scissor{{flipped.x, flipped.y},
	                       {static_cast<uint32_t>(flipped.w), static_cast<uint32_t>(flipped.h)}};
	vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
}

void VulkanNoOpCommandBuffer::begin_pass(const Texture* /* target */, const PassClear& /* clear */) {
}
void VulkanNoOpCommandBuffer::set_viewport(const Recti& /* viewport */) {
}
void VulkanNoOpCommandBuffer::set_scissor(const Recti& /* rect */) {
}
void VulkanNoOpCommandBuffer::disable_scissor() {
}
void VulkanNoOpCommandBuffer::bind_pipeline(const Pipeline* /* pipeline */) {
}
void VulkanNoOpCommandBuffer::bind_descriptor_set(const DescriptorSet* /* set */) {
}
void VulkanNoOpCommandBuffer::bind_vertex_buffer(const Buffer* /* buffer */) {
}
void VulkanNoOpCommandBuffer::draw(const uint32_t /* vertex_offset */, const uint32_t /* vertex_count */) {
}
void VulkanNoOpCommandBuffer::transition(const Texture* /* texture */,
                                         const TextureLayout /* layout */) {
}
void VulkanNoOpCommandBuffer::end_pass() {
}

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
