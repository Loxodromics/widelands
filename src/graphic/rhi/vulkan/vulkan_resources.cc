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

VulkanDescriptorSet::VulkanDescriptorSet(const bool requires_binding)
   : requires_binding_(requires_binding) {
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
     image_(image),
     image_memory_(image_memory),
     view_(view),
     render_pass_(render_pass),
     framebuffer_(framebuffer) {
}

VulkanTexture::~VulkanTexture() {
	vkDestroyFramebuffer(device_, framebuffer_, nullptr);
	vkDestroyRenderPass(device_, render_pass_, nullptr);
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

void VulkanTexture::upload(const void* /* pixels */) {
	throw wexception("Rhi::VulkanTexture::upload: texture upload is WP-16");
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

VkRenderPass VulkanTexture::render_pass() const {
	return render_pass_;
}

VkFramebuffer VulkanTexture::framebuffer() const {
	return framebuffer_;
}

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
