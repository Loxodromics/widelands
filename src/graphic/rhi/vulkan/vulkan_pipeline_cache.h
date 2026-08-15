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

#ifndef WL_GRAPHIC_RHI_VULKAN_VULKAN_PIPELINE_CACHE_H
#define WL_GRAPHIC_RHI_VULKAN_VULKAN_PIPELINE_CACHE_H

#ifdef WL_BUILD_VULKAN

#include <memory>
#include <string>

#include <volk.h>

#include "graphic/rhi/rhi.h"

// The Vulkan pipeline factory (renderer modernization plan, WP-14): loads the
// committed SPIR-V (data/shaders/vulkan/) and the bindings manifest, and
// pre-builds the twelve VkPipeline objects the renderer needs - the pipeline
// catalog's eight programs times their blend states - all against the screen
// render pass (colour + depth). Built once at device startup; nothing draws
// with the pipelines until WP-15, but every boot already exercises the whole
// set, which is what "the pipeline cache builds without validation errors"
// is verified by.
//
// Pipelines are tied to the swapchain image format (they bake in the render
// pass), so a format change on swapchain recreation means rebuilding this
// object. The vertex layouts come from the pipeline catalog (the same
// descriptors the eight programs use - WP-14's acceptance criterion) and the
// attribute locations from the manifest, so the shader source remains the
// single source of truth and a renamed attribute throws here at startup.
namespace Rhi {

class VulkanPipelineCache {
public:
	// Builds everything for the given device and attachment formats. Throws
	// wexception on any failure - a broken shader/data setup must be a clear
	// startup error, not a validation-layer surprise mid-frame.
	VulkanPipelineCache(VkDevice device, VkFormat color_format, VkFormat depth_format);
	~VulkanPipelineCache();

	VulkanPipelineCache(const VulkanPipelineCache&) = delete;
	VulkanPipelineCache& operator=(const VulkanPipelineCache&) = delete;

	// The screen render pass: colour (CLEAR -> PRESENT_SRC) + depth (CLEAR ->
	// DEPTH_STENCIL_ATTACHMENT_OPTIMAL), one subpass.
	VkRenderPass render_pass() const;

	// The pipeline for (program_name, blend); throws on an unknown pair.
	VkPipeline pipeline(const std::string& program_name, const BlendState& blend) const;

	// The descriptor set layout (set 0) for 'program_name'; throws on unknown.
	VkDescriptorSetLayout descriptor_set_layout(const std::string& program_name) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_PIPELINE_CACHE_H
