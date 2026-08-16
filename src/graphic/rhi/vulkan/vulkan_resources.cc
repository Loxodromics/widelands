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

#include "graphic/rhi/vulkan/vulkan_resources.h"

#ifdef WL_BUILD_VULKAN

#include <cstring>
#include <utility>

#include "base/wexception.h"

namespace Rhi {

VulkanPipeline::VulkanPipeline(std::string program_name,
                               const BlendState blend,
                               const bool requires_binding)
   : program_name_(std::move(program_name)),
     blend_(blend),
     requires_binding_(requires_binding),
     handle_(VK_NULL_HANDLE) {
}

VulkanPipeline::VulkanPipeline(std::string program_name,
                               const BlendState blend,
                               const bool requires_binding,
                               const VkPipeline handle)
   : program_name_(std::move(program_name)),
     blend_(blend),
     requires_binding_(requires_binding),
     handle_(handle) {
	if (handle_ == VK_NULL_HANDLE) {
		throw wexception("Vulkan: a directly-handled pipeline needs a valid VkPipeline");
	}
}

const std::string& VulkanPipeline::program_name() const {
	return program_name_;
}

const BlendState& VulkanPipeline::blend() const {
	return blend_;
}

bool VulkanPipeline::requires_binding() const {
	return requires_binding_;
}

VkPipeline VulkanPipeline::handle() const {
	return handle_;
}

VulkanDescriptorSet::VulkanDescriptorSet(std::string program_name,
                                         const ManifestProgram* manifest,
                                         const VkDescriptorSetLayout set_layout,
                                         const VkPipelineLayout pipeline_layout,
                                         const bool requires_binding)
   : program_name_(std::move(program_name)),
     manifest_(manifest),
     set_layout_(set_layout),
     pipeline_layout_(pipeline_layout),
     requires_binding_(requires_binding) {
}

void VulkanDescriptorSet::set_texture(const uint32_t binding, const Texture* texture) {
	textures_[binding] = texture;
}

void VulkanDescriptorSet::set_uniform_buffer(const uint32_t binding,
                                             const Buffer* buffer,
                                             const uint32_t offset,
                                             const uint32_t size) {
	uniform_buffers_[binding] = UniformBinding{buffer, offset, size};
}

const std::string& VulkanDescriptorSet::program_name() const {
	return program_name_;
}

const ManifestProgram* VulkanDescriptorSet::manifest() const {
	return manifest_;
}

VkDescriptorSetLayout VulkanDescriptorSet::set_layout() const {
	return set_layout_;
}

VkPipelineLayout VulkanDescriptorSet::pipeline_layout() const {
	return pipeline_layout_;
}

bool VulkanDescriptorSet::requires_binding() const {
	return requires_binding_;
}

const std::unordered_map<uint32_t, const Texture*>& VulkanDescriptorSet::textures() const {
	return textures_;
}

const std::unordered_map<uint32_t, VulkanDescriptorSet::UniformBinding>&
VulkanDescriptorSet::uniform_buffers() const {
	return uniform_buffers_;
}

namespace {

VkFormat to_vk_format(const TextureFormat format) {
	switch (format) {
	case TextureFormat::kRGBA8:
		return VK_FORMAT_R8G8B8A8_UNORM;
	case TextureFormat::kR8:
		return VK_FORMAT_R8_UNORM;
	}
	NEVER_HERE();
}

}  // namespace

VulkanTexture::VulkanTexture(const VkDevice device,
                             const uint32_t width,
                             const uint32_t height,
                             const TextureFormat format,
                             const TextureFilter /* filter */,
                             const VkSampler sampler,
                             VulkanUploadContext* const upload)
   : device_(device),
     width_(width),
     height_(height),
     format_(to_vk_format(format)),
     image_(VK_NULL_HANDLE),
     image_memory_(VK_NULL_HANDLE),
     view_(VK_NULL_HANDLE),
     sampler_(sampler),
     render_pass_(VK_NULL_HANDLE),
     framebuffer_(VK_NULL_HANDLE),
     upload_(upload),
     current_layout_(VK_IMAGE_LAYOUT_UNDEFINED) {
	// A sampled texture: TRANSFER_DST for upload, SAMPLED for descriptor
	// binding, TRANSFER_SRC so the WP-18 read_back copy (and the headless
	// test's copyback) can read it. No colour-attachment usage - a texture
	// that becomes a render target is created by WP-16b's offscreen path,
	// not here.
	VkImageCreateInfo image_create_info{};
	image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.format = format_;
	image_create_info.extent = {width, height, 1};
	image_create_info.mipLevels = 1;
	image_create_info.arrayLayers = 1;
	image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device, &image_create_info, nullptr, &image_) != VK_SUCCESS) {
		throw wexception("Vulkan: vkCreateImage failed for a %ux%u texture", width, height);
	}

	VkMemoryRequirements memory_requirements{};
	vkGetImageMemoryRequirements(device, image_, &memory_requirements);
	// Device-local memory for the image itself; the staging data lives in the
	// upload arena instead. Picked from the image's own memoryTypeBits, so
	// the allocation always lands on a type the driver allows.
	VkPhysicalDeviceMemoryProperties memory_properties{};
	if (upload == nullptr || upload->physical_device == VK_NULL_HANDLE) {
		throw wexception("Vulkan: no upload context for a sampled texture");
	}
	vkGetPhysicalDeviceMemoryProperties(upload->physical_device, &memory_properties);
	uint32_t memory_type_index = 0;
	bool memory_type_found = false;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
		if (((memory_requirements.memoryTypeBits & (1u << i)) != 0u) &&
		    (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) !=
		       0u) {
			memory_type_index = i;
			memory_type_found = true;
			break;
		}
	}
	if (!memory_type_found) {
		throw wexception("Vulkan: no device-local memory type for a sampled texture");
	}
	VkMemoryAllocateInfo allocate_info{};
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = memory_requirements.size;
	allocate_info.memoryTypeIndex = memory_type_index;
	if (vkAllocateMemory(device, &allocate_info, nullptr, &image_memory_) != VK_SUCCESS) {
		throw wexception("Vulkan: vkAllocateMemory failed for a %ux%u texture", width, height);
	}
	if (vkBindImageMemory(device, image_, image_memory_, 0) != VK_SUCCESS) {
		throw wexception("Vulkan: vkBindImageMemory failed for a %ux%u texture", width, height);
	}

	VkImageViewCreateInfo view_create_info{};
	view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_create_info.image = image_;
	view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_create_info.format = format_;
	view_create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(device, &view_create_info, nullptr, &view_) != VK_SUCCESS) {
		throw wexception("Vulkan: vkCreateImageView failed for a %ux%u texture", width, height);
	}
}

VulkanTexture::VulkanTexture(const VkDevice device,
                             const uint32_t width,
                             const uint32_t height,
                             const VkImage image,
                             const VkDeviceMemory image_memory,
                             const VkImageView view,
                             const VkRenderPass render_pass,
                             const VkFramebuffer framebuffer)
   : device_(device),
     width_(width),
     height_(height),
     format_(VK_FORMAT_UNDEFINED),
     image_(image),
     image_memory_(image_memory),
     view_(view),
     sampler_(VK_NULL_HANDLE),
     render_pass_(render_pass),
     framebuffer_(framebuffer),
     upload_(nullptr),
     current_layout_(VK_IMAGE_LAYOUT_UNDEFINED) {
}

VulkanTexture::~VulkanTexture() {
	if (framebuffer_ != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(device_, framebuffer_, nullptr);
	}
	if (render_pass_ != VK_NULL_HANDLE) {
		vkDestroyRenderPass(device_, render_pass_, nullptr);
	}
	vkDestroyImageView(device_, view_, nullptr);
	vkFreeMemory(device_, image_memory_, nullptr);
	vkDestroyImage(device_, image_, nullptr);
}

uint32_t VulkanTexture::width() const {
	return width_;
}

uint32_t VulkanTexture::height() const {
	return height_;
}

void VulkanTexture::upload(const void* pixels) {
	if (upload_ == nullptr) {
		throw wexception("Rhi::VulkanTexture::upload: no upload context (offscreen target)");
	}
	VulkanUploadContext& context = *upload_;

	// Stage the whole texture through the context's persistent staging
	// buffer and copy it in with a one-shot command buffer from the upload
	// pool. The copy runs immediately (submit + fence wait), so the staging
	// buffer is free for the next upload on return and the texture is ready
	// for sampling the moment upload() returns.
	std::lock_guard<std::mutex> lock(context.mutex);
	const VkDeviceSize row_bytes =
	   static_cast<VkDeviceSize>(width_) * (format_ == VK_FORMAT_R8_UNORM ? 1u : 4u);
	const VkDeviceSize staging_size = row_bytes * height_;

	// Grow the staging buffer on demand (the largest texture upload - the
	// atlas - wins). Host-visible coherent memory, deliberately NOT the
	// per-frame arena: startup uploads run outside the frame loop, so arena
	// regions would accumulate without a reset, and on the NVIDIA box the
	// arena prefers the 246 MB BAR heap, which a full startup's worth of
	// never-recycled staging would exhaust (WP-16).
	if (context.staging_buffer == VK_NULL_HANDLE || context.staging_size < staging_size) {
		if (context.staging_buffer != VK_NULL_HANDLE) {
			vkUnmapMemory(context.device, context.staging_memory);
			vkDestroyBuffer(context.device, context.staging_buffer, nullptr);
			vkFreeMemory(context.device, context.staging_memory, nullptr);
		}
		VkBufferCreateInfo buffer_create_info{};
		buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_create_info.size = staging_size;
		buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(context.device, &buffer_create_info, nullptr,
		                   &context.staging_buffer) != VK_SUCCESS) {
			throw wexception("Vulkan: vkCreateBuffer failed for the upload staging buffer");
		}
		VkMemoryRequirements memory_requirements{};
		vkGetBufferMemoryRequirements(context.device, context.staging_buffer,
		                              &memory_requirements);
		VkPhysicalDeviceMemoryProperties memory_properties{};
		vkGetPhysicalDeviceMemoryProperties(context.physical_device, &memory_properties);
		uint32_t memory_type_index = 0;
		bool memory_type_found = false;
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
			const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
			if (((memory_requirements.memoryTypeBits & (1u << i)) != 0u) &&
			    (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			       (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
				memory_type_index = i;
				memory_type_found = true;
				break;
			}
		}
		if (!memory_type_found) {
			throw wexception("Vulkan: no host-visible coherent memory type for upload staging");
		}
		VkMemoryAllocateInfo allocate_info{};
		allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize = memory_requirements.size;
		allocate_info.memoryTypeIndex = memory_type_index;
		if (vkAllocateMemory(context.device, &allocate_info, nullptr,
		                     &context.staging_memory) != VK_SUCCESS) {
			throw wexception("Vulkan: vkAllocateMemory failed for the upload staging buffer");
		}
		if (vkBindBufferMemory(context.device, context.staging_buffer, context.staging_memory, 0) !=
		    VK_SUCCESS) {
			throw wexception("Vulkan: vkBindBufferMemory failed for the upload staging buffer");
		}
		if (vkMapMemory(context.device, context.staging_memory, 0, staging_size, 0,
		                &context.staging_mapped) != VK_SUCCESS) {
			throw wexception("Vulkan: vkMapMemory failed for the upload staging buffer");
		}
		context.staging_size = staging_size;
	}

	// Row order: a straight copy. The RHI contract (rhi.h §2.4) fixes v = 0
	// to the first row of the uploaded data - under GL that is the bottom of
	// the GL texture, under Vulkan the top of the image, so both sample the
	// same texel at the same interpolated v. The screen-space half of the
	// Vulkan compensation is the committed SPIR-V Y-negation plus the
	// flipped viewport (WP-13/WP-15); the upload itself must not flip again.
	std::memcpy(context.staging_mapped, pixels, staging_size);

	// A one-shot command buffer from the upload pool (never the frame's
	// recording buffer).
	VkCommandBufferAllocateInfo allocate_info{};
	allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate_info.commandPool = context.command_pool;
	allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate_info.commandBufferCount = 1;
	VkCommandBuffer upload_commands = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(context.device, &allocate_info, &upload_commands) != VK_SUCCESS) {
		throw wexception("Vulkan: vkAllocateCommandBuffers failed for a texture upload");
	}
	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(upload_commands, &begin_info) != VK_SUCCESS) {
		throw wexception("Vulkan: vkBeginCommandBuffer failed for a texture upload");
	}

	// UNDEFINED -> TRANSFER_DST_OPTIMAL, copy, TRANSFER_DST_OPTIMAL ->
	// SHADER_READ_ONLY_OPTIMAL: the image is sampled by later frames, so the
	// second transition is not deferred to a future submit.
	const VkImageSubresourceRange subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image_;
	barrier.subresourceRange = subresource;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	vkCmdPipelineBarrier(upload_commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	VkBufferImageCopy copy_region{};
	copy_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy_region.imageExtent = {width_, height_, 1};
	vkCmdCopyBufferToImage(upload_commands, context.staging_buffer, image_,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(upload_commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &barrier);
	if (vkEndCommandBuffer(upload_commands) != VK_SUCCESS) {
		throw wexception("Vulkan: vkEndCommandBuffer failed for a texture upload");
	}

	// The fence is reset before the submit: it starts signaled, so a submit
	// without the reset would make the wait a no-op. The wait makes the
	// staging buffer reusable on return.
	vkResetFences(context.device, 1, &context.fence);
	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &upload_commands;
	if (vkQueueSubmit(context.queue, 1, &submit_info, context.fence) != VK_SUCCESS) {
		throw wexception("Vulkan: vkQueueSubmit failed for a texture upload");
	}
	if (vkWaitForFences(context.device, 1, &context.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
		throw wexception("Vulkan: vkWaitForFences failed for a texture upload");
	}
	vkFreeCommandBuffers(context.device, context.command_pool, 1, &upload_commands);

	current_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanTexture::read_back(uint8_t* /* pixels */) {
	throw wexception("Rhi::VulkanTexture::read_back: texture readback is WP-18");
}

VkImage VulkanTexture::image() const {
	return image_;
}

VkImageView VulkanTexture::view() const {
	return view_;
}

VkSampler VulkanTexture::sampler() const {
	return sampler_;
}

VkRenderPass VulkanTexture::render_pass() const {
	return render_pass_;
}

VkFramebuffer VulkanTexture::framebuffer() const {
	return framebuffer_;
}

VkImageLayout VulkanTexture::current_layout() const {
	return current_layout_;
}

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
