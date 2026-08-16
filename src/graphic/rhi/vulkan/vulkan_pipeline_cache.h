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

namespace Rhi {

struct ManifestProgram;

// The Vulkan pipeline factory (renderer modernization plan, WP-14): loads the
// committed SPIR-V (data/shaders/vulkan/) and the bindings manifest, and
// pre-builds the VkPipeline objects the renderer needs - the pipeline
// catalog's eight programs times their blend states - all against the screen
// render pass (colour + depth). Built once at device startup; the command
// buffer resolves its draws through here (WP-15).
//
// Since WP-16b it also owns the immediate render-to-texture side: the shared
// colour-only offscreen render pass (load, one RGBA8 attachment, leaving the
// image shader-read-only) and the offscreen pipelines for the three programs
// that draw into textures (blit, fill_rect, draw_line), with the depth test
// off - the GL offscreen FBO has no depth attachment, so there is nothing to
// test against and draws resolve in submission order on both backends. The
// descriptor set and pipeline layouts are render-pass independent and shared
// with the screen variants.
//
// Pipelines are tied to the swapchain image format (they bake in the render
// pass), so a format change on swapchain recreation means rebuilding this
// object. The vertex layouts come from the pipeline catalog (the same
// descriptors the eight programs use - WP-14's acceptance criterion) and the
// attribute locations from the manifest, so the shader source remains the
// single source of truth and a renamed attribute throws here at startup.
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

	// The shared offscreen render pass (WP-16b): one RGBA8 colour attachment,
	// LOAD -> SHADER_READ_ONLY, no depth. The load is baked in because every
	// immediate render-to-texture pass draws over existing contents
	// (texture.cc passes clear=false; a "clear" is a Copy fill_rect).
	VkRenderPass offscreen_render_pass() const;

	// The pipeline for (program_name, blend); throws on an unknown pair.
	VkPipeline pipeline(const std::string& program_name, const BlendState& blend) const;

	// The offscreen variant of a pipeline (WP-16b): same program and blend,
	// built against the offscreen render pass with the depth test off.
	// Throws for a program that never draws into textures.
	VkPipeline pipeline_for_offscreen(const std::string& program_name, const BlendState& blend) const;

	// The descriptor set layout (set 0) for 'program_name'; throws on unknown.
	VkDescriptorSetLayout descriptor_set_layout(const std::string& program_name) const;

	// The pipeline layout for 'program_name' (the layout the command buffer
	// binds descriptor sets against); throws on unknown.
	VkPipelineLayout pipeline_layout(const std::string& program_name) const;

	// The manifest entry for 'program_name' - the binding translation the
	// command buffer needs when writing descriptor sets (WP-16: the RHI's
	// per-type binding indices map onto the manifest's shared Vulkan
	// bindings). Never null for a program the catalog knows.
	const ManifestProgram* manifest_program(const std::string& program_name) const;

	// Whether the program's descriptor set layout declares any bindings (any
	// sampler or uniform block in the manifest). The command buffer needs it
	// to know whether a draw requires a bound descriptor set (WP-16); a
	// pipeline whose layout is empty (fill_rect, draw_line) draws without
	// one.
	bool has_descriptor_bindings(const std::string& program_name) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_PIPELINE_CACHE_H
