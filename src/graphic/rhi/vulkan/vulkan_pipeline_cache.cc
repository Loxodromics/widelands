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

#include "graphic/rhi/vulkan/vulkan_pipeline_cache.h"

#ifdef WL_BUILD_VULKAN

#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/log.h"
#include "base/wexception.h"
#include "graphic/rhi/pipeline_catalog.h"
#include "graphic/rhi/vulkan/vulkan_manifest.h"
#include "io/fileread.h"
#include "io/filesystem/layered_filesystem.h"

namespace Rhi {

namespace {

// Maps the RHI's vertex formats to their Vulkan float equivalents. All
// renderer vertices are floats; no normalized or integer attributes exist.
VkFormat to_vk_format(const VertexFormat format) {
	switch (format) {
	case VertexFormat::kFloat:
		return VK_FORMAT_R32_SFLOAT;
	case VertexFormat::kVec2:
		return VK_FORMAT_R32G32_SFLOAT;
	case VertexFormat::kVec3:
		return VK_FORMAT_R32G32B32_SFLOAT;
	case VertexFormat::kVec4:
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	}
	NEVER_HERE();
}

VkPrimitiveTopology to_vk_topology(const PrimitiveTopology topology) {
	switch (topology) {
	case PrimitiveTopology::kTriangleList:
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	case PrimitiveTopology::kLineList:
		return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	}
	NEVER_HERE();
}

VkBlendFactor to_vk_blend_factor(const BlendFactor factor) {
	switch (factor) {
	case BlendFactor::kZero:
		return VK_BLEND_FACTOR_ZERO;
	case BlendFactor::kOne:
		return VK_BLEND_FACTOR_ONE;
	case BlendFactor::kSrcAlpha:
		return VK_BLEND_FACTOR_SRC_ALPHA;
	case BlendFactor::kOneMinusSrcAlpha:
		return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}
	NEVER_HERE();
}

VkBlendOp to_vk_blend_op(const BlendOp op) {
	switch (op) {
	case BlendOp::kAdd:
		return VK_BLEND_OP_ADD;
	case BlendOp::kReverseSubtract:
		return VK_BLEND_OP_REVERSE_SUBTRACT;
	}
	NEVER_HERE();
}

VkCompareOp to_vk_compare_op(const CompareOp op) {
	switch (op) {
	case CompareOp::kLess:
		return VK_COMPARE_OP_LESS;
	case CompareOp::kLessOrEqual:
		return VK_COMPARE_OP_LESS_OR_EQUAL;
	case CompareOp::kAlways:
		return VK_COMPARE_OP_ALWAYS;
	}
	NEVER_HERE();
}

bool same_blend(const BlendState& a, const BlendState& b) {
	return a.src_factor == b.src_factor && a.dst_factor == b.dst_factor && a.op == b.op;
}

// Reads one committed SPIR-V module through the layered filesystem. The
// committed artifacts are a build-time-checked mirror of the shader sources
// (WP-13), so the game never compiles shaders at runtime.
std::vector<uint32_t> load_spirv(const std::string& program_name, const char* stage) {
	const std::string path = "shaders/vulkan/" + program_name + "." + stage + ".spv";
	FileRead fr;
	fr.open(*g_fs, path);
	std::vector<uint32_t> words((fr.get_size() + 3) / 4, 0);
	if (fr.get_size() % 4 != 0) {
		throw wexception("SPIR-V module '%s' has a size that is not a multiple of 4", path.c_str());
	}
	std::memcpy(words.data(), fr.data(0), fr.get_size());
	fr.close();
	return words;
}

}  // namespace

struct VulkanPipelineCache::Impl {
	Impl(VkDevice device, const VkFormat color_format, const VkFormat depth_format);
	~Impl();

	VkDevice device = VK_NULL_HANDLE;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
	VkRenderPass render_pass = VK_NULL_HANDLE;

	// The shared offscreen render pass (WP-16b): one RGBA8 attachment,
	// LOAD -> SHADER_READ_ONLY, no depth. Independent of the swapchain
	// format, so the offscreen pipelines are built once per cache rebuild.
	VkRenderPass offscreen_render_pass = VK_NULL_HANDLE;

	// The parsed bindings manifest (WP-13): the single source of truth for
	// descriptor layouts and attribute locations, kept for the whole device
	// lifetime so the command buffer can translate binding indices (WP-16).
	VulkanManifest manifest;

	// Per program (all in descriptor set 0): the set layout, the pipeline
	// layout, and the pipelines keyed by blend state. Keys are the program
	// name; every program in the pipeline catalog is present.
	struct ProgramPipelines {
		VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		std::vector<std::pair<BlendState, VkPipeline>> pipelines;
		// Whether the descriptor set layout declares any bindings (the
		// command buffer needs to know whether a draw requires a bound set).
		bool has_bindings = false;
	};
	std::map<std::string, ProgramPipelines> programs;

	// The offscreen pipeline variants (WP-16b): the three programs that draw
	// into textures, keyed by program name, each against the offscreen
	// render pass with the depth test off. The descriptor set and pipeline
	// layouts are shared with the screen variants (they are render-pass
	// independent), so lookups above still resolve there.
	std::map<std::string, std::vector<std::pair<BlendState, VkPipeline>>> offscreen_pipelines;

	VkDescriptorSetLayout make_descriptor_set_layout(const ManifestProgram& program) const;
	VkPipeline create_pipeline(const PipelineDescriptor& desc,
	                           const ManifestProgram& manifest_program,
	                           VkPipelineLayout pipeline_layout,
	                           VkRenderPass target_render_pass,
	                           bool depth_enabled) const;
};

VulkanPipelineCache::Impl::Impl(const VkDevice init_device,
                                const VkFormat color_format,
                                const VkFormat depth_format)
   : device(init_device) {
	// The manifest is the single source of truth for descriptor layouts and
	// attribute locations (WP-13); the catalog is the single source of truth
	// for vertex layouts, topology and blend state (WP-14). Both are loaded
	// here and must agree - a mismatch is a startup error, not a runtime
	// mystery.
	manifest = load_manifest();

	VkPipelineCacheCreateInfo cache_create_info{};
	cache_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (vkCreatePipelineCache(device, &cache_create_info, nullptr, &pipeline_cache) != VK_SUCCESS) {
		throw wexception("Vulkan: vkCreatePipelineCache failed");
	}

	// The screen render pass: one colour attachment (the swapchain image,
	// cleared every frame, presented afterwards) plus one depth attachment
	// (cleared to 1.0 = far, matching RHI_INTERFACE.md §2.5). The initial
	// layouts are UNDEFINED - loadOp CLEAR discards the previous contents, so
	// no pre-pass barrier is needed for either attachment.
	VkAttachmentDescription attachments[2] {};
	attachments[0].format = color_format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	attachments[1].format = depth_format;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_reference{};
	color_reference.attachment = 0;
	color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference depth_reference{};
	depth_reference.attachment = 1;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_reference;
	subpass.pDepthStencilAttachment = &depth_reference;

	// The standard external -> subpass dependency: the clear of both
	// attachments must happen after any earlier use of the images.
	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.srcAccessMask = 0;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo render_pass_create_info{};
	render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_create_info.attachmentCount = 2;
	render_pass_create_info.pAttachments = attachments;
	render_pass_create_info.subpassCount = 1;
	render_pass_create_info.pSubpasses = &subpass;
	render_pass_create_info.dependencyCount = 1;
	render_pass_create_info.pDependencies = &dependency;
	if (vkCreateRenderPass(device, &render_pass_create_info, nullptr, &render_pass) != VK_SUCCESS) {
		throw wexception("Vulkan: vkCreateRenderPass failed");
	}

	// Descriptor set layouts, pipeline layouts, and the pipelines themselves
	// for every pipeline-catalog entry.
	for (const PipelineDescriptor& desc : pipeline_catalog()) {
		const ManifestProgram* manifest_program = manifest.find_program(desc.program_name);
		if (manifest_program == nullptr) {
			throw wexception(
			   "Vulkan: program '%s' is in the pipeline catalog but not in the bindings manifest",
			   desc.program_name.c_str());
		}
		ProgramPipelines& program = programs[desc.program_name];
		if (program.descriptor_set_layout == VK_NULL_HANDLE) {
			program.descriptor_set_layout = make_descriptor_set_layout(*manifest_program);
			program.has_bindings = !manifest_program->samplers.empty() ||
			                       !manifest_program->uniform_blocks.empty();
			VkPipelineLayoutCreateInfo pipeline_layout_create_info{};
			pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipeline_layout_create_info.setLayoutCount = 1;
			pipeline_layout_create_info.pSetLayouts = &program.descriptor_set_layout;
			if (vkCreatePipelineLayout(
			       device, &pipeline_layout_create_info, nullptr, &program.pipeline_layout) !=
			    VK_SUCCESS) {
				throw wexception("Vulkan: vkCreatePipelineLayout failed for program '%s'",
				                 desc.program_name.c_str());
			}
		}
		program.pipelines.emplace_back(desc.blend,
		                               create_pipeline(desc, *manifest_program,
		                                               program.pipeline_layout, render_pass,
		                                               desc.depth.test_enabled));
		verb_log_info("Graphics: Vulkan: Pipeline: %s (blend %d)\n", desc.program_name.c_str(),
		              static_cast<int>(desc.blend.src_factor) * 100 +
		                 static_cast<int>(desc.blend.dst_factor) * 10 +
		                 static_cast<int>(desc.blend.op));
	}

	// The offscreen render pass (WP-16b): one RGBA8 colour attachment - the
	// fixed format graphic::Texture creates its Vulkan textures with - loaded
	// on entry (the immediate render-to-texture path draws over existing
	// contents; texture.cc passes clear=false and a "clear" is a Copy
	// fill_rect) and left shader-read-only, so the image is sampleable the
	// moment end_pass runs. No depth attachment: the GL offscreen FBO has
	// none, so there is nothing to depth-test against on either backend and
	// draws resolve in submission order.
	{
		VkAttachmentDescription attachment{};
		attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkAttachmentReference color_reference{};
		color_reference.attachment = 0;
		color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color_reference;

		// The LOAD must wait for the image's previous use (a color write
		// from an earlier offscreen pass or a shader read from sampling),
		// and the writes must be visible to the sampling that follows the
		// pass - the finalLayout transition alone does not order memory, so
		// the write -> fragment-shader-read dependency lives here.
		VkSubpassDependency dependencies[2] {};
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
		                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		                                VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo render_pass_create_info{};
		render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		render_pass_create_info.attachmentCount = 1;
		render_pass_create_info.pAttachments = &attachment;
		render_pass_create_info.subpassCount = 1;
		render_pass_create_info.pSubpasses = &subpass;
		render_pass_create_info.dependencyCount = 2;
		render_pass_create_info.pDependencies = dependencies;
		if (vkCreateRenderPass(device, &render_pass_create_info, nullptr,
		                       &offscreen_render_pass) != VK_SUCCESS) {
			throw wexception("Vulkan: vkCreateRenderPass failed for the offscreen pass");
		}
	}

	// The offscreen pipelines: the three programs texture.cc's do_* methods
	// draw with, against the offscreen pass, depth test off. The descriptor
	// set and pipeline layouts are the ones already built above (they are
	// render-pass independent).
	for (const PipelineDescriptor& desc : pipeline_catalog()) {
		if (desc.program_name != "blit" && desc.program_name != "fill_rect" &&
		    desc.program_name != "draw_line") {
			continue;
		}
		const ManifestProgram* manifest_program = manifest.find_program(desc.program_name);
		// Existence was asserted in the screen loop above.
		const ProgramPipelines& program = programs.at(desc.program_name);
		offscreen_pipelines[desc.program_name].emplace_back(
		   desc.blend,
		   create_pipeline(desc, *manifest_program, program.pipeline_layout, offscreen_render_pass,
		                   false));
		verb_log_info("Graphics: Vulkan: Offscreen pipeline: %s (blend %d)\n",
		              desc.program_name.c_str(), static_cast<int>(desc.blend.src_factor) * 100 +
		                                             static_cast<int>(desc.blend.dst_factor) * 10 +
		                                             static_cast<int>(desc.blend.op));
	}
}

VulkanPipelineCache::Impl::~Impl() {
	for (auto& entry : programs) {
		for (auto& variant : entry.second.pipelines) {
			vkDestroyPipeline(device, variant.second, nullptr);
		}
		if (entry.second.pipeline_layout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, entry.second.pipeline_layout, nullptr);
		}
		if (entry.second.descriptor_set_layout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, entry.second.descriptor_set_layout, nullptr);
		}
	}
	for (auto& entry : offscreen_pipelines) {
		for (auto& variant : entry.second) {
			vkDestroyPipeline(device, variant.second, nullptr);
		}
	}
	if (offscreen_render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(device, offscreen_render_pass, nullptr);
	}
	if (render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(device, render_pass, nullptr);
	}
	if (pipeline_cache != VK_NULL_HANDLE) {
		vkDestroyPipelineCache(device, pipeline_cache, nullptr);
	}
}

VkDescriptorSetLayout
VulkanPipelineCache::Impl::make_descriptor_set_layout(const ManifestProgram& program) const {
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	// Samplers first (fragment-stage only), then the uniform blocks with the
	// stage flags the manifest records. Binding numbers come from the manifest,
	// which assigns them from one shared counter, so they never collide.
	for (const auto& sampler : program.samplers) {
		VkDescriptorSetLayoutBinding binding{};
		binding.binding = sampler.second;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		bindings.push_back(binding);
	}
	for (const ManifestUniformBlock& block : program.uniform_blocks) {
		VkDescriptorSetLayoutBinding binding{};
		binding.binding = block.binding;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		binding.descriptorCount = 1;
		VkShaderStageFlags stages = 0;
		if (block.vertex_stage) {
			stages |= VK_SHADER_STAGE_VERTEX_BIT;
		}
		if (block.fragment_stage) {
			stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		binding.stageFlags = stages;
		bindings.push_back(binding);
	}

	VkDescriptorSetLayoutCreateInfo layout_create_info{};
	layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_create_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_create_info.pBindings = bindings.data();
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	if (vkCreateDescriptorSetLayout(device, &layout_create_info, nullptr, &layout) != VK_SUCCESS) {
		throw wexception("Vulkan: vkCreateDescriptorSetLayout failed");
	}
	return layout;
}

VkPipeline VulkanPipelineCache::Impl::create_pipeline(const PipelineDescriptor& desc,
                                                       const ManifestProgram& manifest_program,
                                                       const VkPipelineLayout pipeline_layout,
                                                       const VkRenderPass target_render_pass,
                                                       const bool depth_enabled) const {
	// Shader stages from the committed SPIR-V. Modules are destroyed right
	// after pipeline creation - the pipeline keeps everything it needs.
	const std::vector<uint32_t> vertex_spirv = load_spirv(desc.program_name, "vert");
	const std::vector<uint32_t> fragment_spirv = load_spirv(desc.program_name, "frag");

	const auto make_module = [this](const std::vector<uint32_t>& words) {
		VkShaderModuleCreateInfo module_create_info{};
		module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_create_info.codeSize = words.size() * sizeof(uint32_t);
		module_create_info.pCode = words.data();
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &module_create_info, nullptr, &module) != VK_SUCCESS) {
			throw wexception("Vulkan: vkCreateShaderModule failed");
		}
		return module;
	};
	VkShaderModule vertex_module = make_module(vertex_spirv);
	VkShaderModule fragment_module = make_module(fragment_spirv);

	VkPipelineShaderStageCreateInfo stages[2] {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex_module;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment_module;
	stages[1].pName = "main";

	// Vertex input: one binding at rate VERTEX with the catalog's stride;
	// each attribute's location resolves through the manifest, which records
	// the authored layout(location=N) - the shader stays the single source of
	// truth, and a renamed attribute throws here (the F7 lesson).
	std::vector<VkVertexInputAttributeDescription> attributes;
	attributes.reserve(desc.vertex_layout.attributes.size());
	for (const VertexAttribute& attribute : desc.vertex_layout.attributes) {
		uint32_t location = 0;
		bool found = false;
		for (const auto& manifest_attribute : manifest_program.attributes) {
			if (manifest_attribute.first == attribute.name) {
				location = manifest_attribute.second;
				found = true;
				break;
			}
		}
		if (!found) {
			throw wexception("Vulkan: attribute '%s' of program '%s' is not in the bindings "
			                 "manifest (is the committed manifest stale?)",
			                 attribute.name.c_str(), desc.program_name.c_str());
		}
		VkVertexInputAttributeDescription description{};
		description.location = location;
		description.binding = 0;
		description.format = to_vk_format(attribute.format);
		description.offset = attribute.offset;
		attributes.push_back(description);
	}

	VkVertexInputBindingDescription binding{};
	binding.binding = 0;
	binding.stride = desc.vertex_layout.stride;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkPipelineVertexInputStateCreateInfo vertex_input{};
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
	vertex_input.pVertexAttributeDescriptions = attributes.data();

	VkPipelineInputAssemblyStateCreateInfo input_assembly{};
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = to_vk_topology(desc.topology);
	input_assembly.primitiveRestartEnable = VK_FALSE;

	// Viewport and scissor are dynamic: the pipelines survive swapchain
	// resizes, and WP-15 records the per-frame values (with the canonical ->
	// Vulkan origin compensation from RHI_INTERFACE.md §2.4).
	const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state{};
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2;
	dynamic_state.pDynamicStates = dynamic_states;

	VkPipelineViewportStateCreateInfo viewport_state{};
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;

	// No face culling (GL never enables it), line width 1.0 (grid draws
	// GL_LINES at the default width).
	VkPipelineRasterizationStateCreateInfo rasterization{};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.depthClampEnable = VK_FALSE;
	rasterization.rasterizerDiscardEnable = VK_FALSE;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// The blend state: opaque disables blending, the other three map 1:1 onto
	// the named RHI constants (RHI_INTERFACE.md §4). Colour and alpha always
	// share factors; none of the four states split them.
	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (same_blend(desc.blend, kBlendOpaque)) {
		blend_attachment.blendEnable = VK_FALSE;
	} else {
		blend_attachment.blendEnable = VK_TRUE;
		blend_attachment.srcColorBlendFactor = to_vk_blend_factor(desc.blend.src_factor);
		blend_attachment.dstColorBlendFactor = to_vk_blend_factor(desc.blend.dst_factor);
		blend_attachment.colorBlendOp = to_vk_blend_op(desc.blend.op);
		blend_attachment.srcAlphaBlendFactor = to_vk_blend_factor(desc.blend.src_factor);
		blend_attachment.dstAlphaBlendFactor = to_vk_blend_factor(desc.blend.dst_factor);
		blend_attachment.alphaBlendOp = to_vk_blend_op(desc.blend.op);
	}
	VkPipelineColorBlendStateCreateInfo color_blend{};
	color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blend.attachmentCount = 1;
	color_blend.pAttachments = &blend_attachment;

	// Depth: test and write on, LESS_OR_EQUAL - the renderer's one depth state
	// (the blended pass writes depth too; reproduced, not "cleaned up"). The
	// offscreen variants (WP-16b) disable it: the offscreen render pass has no
	// depth attachment (the GL offscreen FBO has none either), and a pipeline
	// that tests depth against a nonexistent attachment is a validation error.
	VkPipelineDepthStencilStateCreateInfo depth_stencil{};
	depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil.depthTestEnable = depth_enabled ? (desc.depth.test_enabled ? VK_TRUE : VK_FALSE) :
	                                                VK_FALSE;
	depth_stencil.depthWriteEnable = depth_enabled ? (desc.depth.write_enabled ? VK_TRUE : VK_FALSE) :
	                                                 VK_FALSE;
	depth_stencil.depthCompareOp = to_vk_compare_op(desc.depth.compare_op);
	depth_stencil.depthBoundsTestEnable = VK_FALSE;
	depth_stencil.stencilTestEnable = VK_FALSE;
	depth_stencil.minDepthBounds = 0.0f;
	depth_stencil.maxDepthBounds = 1.0f;

	VkGraphicsPipelineCreateInfo pipeline_create_info{};
	pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_create_info.stageCount = 2;
	pipeline_create_info.pStages = stages;
	pipeline_create_info.pVertexInputState = &vertex_input;
	pipeline_create_info.pInputAssemblyState = &input_assembly;
	pipeline_create_info.pViewportState = &viewport_state;
	pipeline_create_info.pRasterizationState = &rasterization;
	pipeline_create_info.pMultisampleState = &multisampling;
	pipeline_create_info.pDepthStencilState = &depth_stencil;
	pipeline_create_info.pColorBlendState = &color_blend;
	pipeline_create_info.pDynamicState = &dynamic_state;
	pipeline_create_info.layout = pipeline_layout;
	pipeline_create_info.renderPass = target_render_pass;
	pipeline_create_info.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result = vkCreateGraphicsPipelines(
	   device, pipeline_cache, 1, &pipeline_create_info, nullptr, &pipeline);
	vkDestroyShaderModule(device, vertex_module, nullptr);
	vkDestroyShaderModule(device, fragment_module, nullptr);
	if (result != VK_SUCCESS) {
		throw wexception("Vulkan: vkCreateGraphicsPipelines failed for program '%s'",
		                 desc.program_name.c_str());
	}
	return pipeline;
}

VulkanPipelineCache::VulkanPipelineCache(const VkDevice device,
                                         const VkFormat color_format,
                                         const VkFormat depth_format)
   : impl_(std::make_unique<Impl>(device, color_format, depth_format)) {
}

VulkanPipelineCache::~VulkanPipelineCache() = default;

VkRenderPass VulkanPipelineCache::render_pass() const {
	return impl_->render_pass;
}

VkRenderPass VulkanPipelineCache::offscreen_render_pass() const {
	return impl_->offscreen_render_pass;
}

VkPipeline
VulkanPipelineCache::pipeline(const std::string& program_name, const BlendState& blend) const {
	const auto program_it = impl_->programs.find(program_name);
	if (program_it == impl_->programs.end()) {
		throw wexception("Vulkan: no pipelines built for program '%s'", program_name.c_str());
	}
	for (const auto& variant : program_it->second.pipelines) {
		if (same_blend(variant.first, blend)) {
			return variant.second;
		}
	}
	throw wexception("Vulkan: no pipeline for program '%s' with the requested blend state",
	                 program_name.c_str());
}

VkPipeline VulkanPipelineCache::pipeline_for_offscreen(const std::string& program_name,
                                                       const BlendState& blend) const {
	const auto program_it = impl_->offscreen_pipelines.find(program_name);
	if (program_it == impl_->offscreen_pipelines.end()) {
		throw wexception("Vulkan: no offscreen pipelines built for program '%s' (it never draws "
		                 "into textures)",
		                 program_name.c_str());
	}
	for (const auto& variant : program_it->second) {
		if (same_blend(variant.first, blend)) {
			return variant.second;
		}
	}
	throw wexception(
	   "Vulkan: no offscreen pipeline for program '%s' with the requested blend state",
	   program_name.c_str());
}

VkDescriptorSetLayout
VulkanPipelineCache::descriptor_set_layout(const std::string& program_name) const {
	const auto program_it = impl_->programs.find(program_name);
	if (program_it == impl_->programs.end()) {
		throw wexception("Vulkan: no descriptor set layout for program '%s'", program_name.c_str());
	}
	return program_it->second.descriptor_set_layout;
}

VkPipelineLayout VulkanPipelineCache::pipeline_layout(const std::string& program_name) const {
	const auto program_it = impl_->programs.find(program_name);
	if (program_it == impl_->programs.end()) {
		throw wexception("Vulkan: no pipeline layout for program '%s'", program_name.c_str());
	}
	return program_it->second.pipeline_layout;
}

const ManifestProgram*
VulkanPipelineCache::manifest_program(const std::string& program_name) const {
	return impl_->manifest.find_program(program_name);
}

bool VulkanPipelineCache::has_descriptor_bindings(const std::string& program_name) const {
	const auto program_it = impl_->programs.find(program_name);
	if (program_it == impl_->programs.end()) {
		throw wexception("Vulkan: no pipelines built for program '%s'", program_name.c_str());
	}
	// The layout was built from the manifest, so asking whether the program
	// has any samplers/UBOs is cheaper than reflecting over the layout.
	return program_it->second.has_bindings;
}

}  // namespace Rhi

#else  // WL_BUILD_VULKAN

// Builds without Vulkan support never construct a pipeline cache: the stub
// VulkanDevice (vulkan_device.cc) throws in its constructor, so none of this
// is ever called, and the translation unit compiles without the Vulkan
// headers (the class itself is only declared under WL_BUILD_VULKAN).

#endif  // WL_BUILD_VULKAN
