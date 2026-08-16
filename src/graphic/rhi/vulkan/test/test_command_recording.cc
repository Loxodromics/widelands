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

#include "base/test.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <volk.h>

#include "graphic/rhi/vulkan/vulkan_buffer.h"
#include "graphic/rhi/vulkan/vulkan_command_buffer.h"
#include "graphic/rhi/vulkan/vulkan_resources.h"

// The headless pixel gate for WP-15's recording path (renderer modernization
// plan): a windowless Vulkan device records real Rhi::VulkanCommandBuffer draws
// into a 64x64 offscreen target, submits, copies the image back to a
// host-visible buffer and asserts on pixels. This pins the three things a
// boot smoke test cannot: vertex upload through the staging arena, the
// canonical -> Vulkan viewport/scissor compensation (the SPIR-V negates
// gl_Position.y, so the viewport must compensate with a negative height),
// and the WP-15 draw-skip rule (pipelines whose descriptor layouts have
// bindings draw nothing until WP-16).
//
// The fill_rect pipeline is built from the committed SPIR-V modules
// (data/shaders/vulkan/fill_rect.{vert,frag}.spv) embedded below, because
// the test framework has no layered filesystem to load them from. They are
// byte-for-byte what the build check (wl_spirv_check) pins against the
// sources; the attribute locations (0 = attr_position, 1 = attr_color) come
// from the committed bindings.json.

// data/shaders/vulkan/fill_rect.vert.spv
constexpr uint32_t kFillRectVertSpirv[] = {
	0x07230203u, 0x00010000u, 0x0008000bu, 0x00000031u, 0x00000000u, 0x00020011u,
	0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
	0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0009000fu, 0x00000000u,
	0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000bu, 0x0000000du, 0x00000014u,
	0x00000019u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u,
	0x6e69616du, 0x00000000u, 0x00050005u, 0x00000006u, 0x6d5f6c77u, 0x286e6961u,
	0x00000000u, 0x00050005u, 0x0000000bu, 0x5f726176u, 0x6f6c6f63u, 0x00000072u,
	0x00050005u, 0x0000000du, 0x72747461u, 0x6c6f635fu, 0x0000726fu, 0x00060005u,
	0x00000012u, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u,
	0x00000012u, 0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u,
	0x00000012u, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u, 0x00000000u,
	0x00070006u, 0x00000012u, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u,
	0x0065636eu, 0x00070006u, 0x00000012u, 0x00000003u, 0x435f6c67u, 0x446c6c75u,
	0x61747369u, 0x0065636eu, 0x00030005u, 0x00000014u, 0x00000000u, 0x00060005u,
	0x00000019u, 0x72747461u, 0x736f705fu, 0x6f697469u, 0x0000006eu, 0x00040047u,
	0x0000000bu, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000du, 0x0000001eu,
	0x00000001u, 0x00030047u, 0x00000012u, 0x00000002u, 0x00050048u, 0x00000012u,
	0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x00000012u, 0x00000001u,
	0x0000000bu, 0x00000001u, 0x00050048u, 0x00000012u, 0x00000002u, 0x0000000bu,
	0x00000003u, 0x00050048u, 0x00000012u, 0x00000003u, 0x0000000bu, 0x00000004u,
	0x00040047u, 0x00000019u, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u,
	0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000008u, 0x00000020u,
	0x00040017u, 0x00000009u, 0x00000008u, 0x00000004u, 0x00040020u, 0x0000000au,
	0x00000003u, 0x00000009u, 0x0004003bu, 0x0000000au, 0x0000000bu, 0x00000003u,
	0x00040020u, 0x0000000cu, 0x00000001u, 0x00000009u, 0x0004003bu, 0x0000000cu,
	0x0000000du, 0x00000001u, 0x00040015u, 0x0000000fu, 0x00000020u, 0x00000000u,
	0x0004002bu, 0x0000000fu, 0x00000010u, 0x00000001u, 0x0004001cu, 0x00000011u,
	0x00000008u, 0x00000010u, 0x0006001eu, 0x00000012u, 0x00000009u, 0x00000008u,
	0x00000011u, 0x00000011u, 0x00040020u, 0x00000013u, 0x00000003u, 0x00000012u,
	0x0004003bu, 0x00000013u, 0x00000014u, 0x00000003u, 0x00040015u, 0x00000015u,
	0x00000020u, 0x00000001u, 0x0004002bu, 0x00000015u, 0x00000016u, 0x00000000u,
	0x00040017u, 0x00000017u, 0x00000008u, 0x00000003u, 0x00040020u, 0x00000018u,
	0x00000001u, 0x00000017u, 0x0004003bu, 0x00000018u, 0x00000019u, 0x00000001u,
	0x0004002bu, 0x00000008u, 0x0000001bu, 0x3f800000u, 0x00040020u, 0x00000022u,
	0x00000003u, 0x00000008u, 0x0004002bu, 0x0000000fu, 0x00000027u, 0x00000002u,
	0x0004002bu, 0x0000000fu, 0x0000002au, 0x00000003u, 0x0004002bu, 0x00000008u,
	0x0000002eu, 0x3f000000u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
	0x00000003u, 0x000200f8u, 0x00000005u, 0x00040039u, 0x00000002u, 0x00000021u,
	0x00000006u, 0x00060041u, 0x00000022u, 0x00000023u, 0x00000014u, 0x00000016u,
	0x00000010u, 0x0004003du, 0x00000008u, 0x00000024u, 0x00000023u, 0x0004007fu,
	0x00000008u, 0x00000025u, 0x00000024u, 0x00060041u, 0x00000022u, 0x00000026u,
	0x00000014u, 0x00000016u, 0x00000010u, 0x0003003eu, 0x00000026u, 0x00000025u,
	0x00060041u, 0x00000022u, 0x00000028u, 0x00000014u, 0x00000016u, 0x00000027u,
	0x0004003du, 0x00000008u, 0x00000029u, 0x00000028u, 0x00060041u, 0x00000022u,
	0x0000002bu, 0x00000014u, 0x00000016u, 0x0000002au, 0x0004003du, 0x00000008u,
	0x0000002cu, 0x0000002bu, 0x00050081u, 0x00000008u, 0x0000002du, 0x00000029u,
	0x0000002cu, 0x00050085u, 0x00000008u, 0x0000002fu, 0x0000002du, 0x0000002eu,
	0x00060041u, 0x00000022u, 0x00000030u, 0x00000014u, 0x00000016u, 0x00000027u,
	0x0003003eu, 0x00000030u, 0x0000002fu, 0x000100fdu, 0x00010038u, 0x00050036u,
	0x00000002u, 0x00000006u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000007u,
	0x0004003du, 0x00000009u, 0x0000000eu, 0x0000000du, 0x0003003eu, 0x0000000bu,
	0x0000000eu, 0x0004003du, 0x00000017u, 0x0000001au, 0x00000019u, 0x00050051u,
	0x00000008u, 0x0000001cu, 0x0000001au, 0x00000000u, 0x00050051u, 0x00000008u,
	0x0000001du, 0x0000001au, 0x00000001u, 0x00050051u, 0x00000008u, 0x0000001eu,
	0x0000001au, 0x00000002u, 0x00070050u, 0x00000009u, 0x0000001fu, 0x0000001cu,
	0x0000001du, 0x0000001eu, 0x0000001bu, 0x00050041u, 0x0000000au, 0x00000020u,
	0x00000014u, 0x00000016u, 0x0003003eu, 0x00000020u, 0x0000001fu, 0x000100fdu,
	0x00010038u,
};

// data/shaders/vulkan/fill_rect.frag.spv
constexpr uint32_t kFillRectFragSpirv[] = {
	0x07230203u, 0x00010000u, 0x0008000bu, 0x0000000du, 0x00000000u, 0x00020011u,
	0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
	0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000004u,
	0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x0000000bu, 0x00030010u,
	0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u,
	0x00000004u, 0x6e69616du, 0x00000000u, 0x00050005u, 0x00000009u, 0x67617266u,
	0x6c6f635fu, 0x0000726fu, 0x00050005u, 0x0000000bu, 0x5f726176u, 0x6f6c6f63u,
	0x00000072u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u,
	0x0000000bu, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u,
	0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
	0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u,
	0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00040020u,
	0x0000000au, 0x00000001u, 0x00000007u, 0x0004003bu, 0x0000000au, 0x0000000bu,
	0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
	0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x0000000cu, 0x0000000bu,
	0x0003003eu, 0x00000009u, 0x0000000cu, 0x000100fdu, 0x00010038u,
};

namespace {

// The render target size: small enough that a full-frame copyback is cheap,
// large enough for non-trivial viewport/scissor rectangles.
constexpr uint32_t kTargetSize = 64;

// The fill_rect vertex layout (pipeline catalog: loc0 vec3 position,
// loc1 vec4 color), 28 bytes per vertex.
struct Vertex {
	float x;
	float y;
	float z;
	float r;
	float g;
	float b;
	float a;
};

// One validation error counter, fed by the debug messenger below.
int g_validation_errors = 0;

VkBool32 VKAPI_CALL
validation_counter(const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                   VkDebugUtilsMessageTypeFlagsEXT /* type */,
                   const VkDebugUtilsMessengerCallbackDataEXT* data,
                   void* /* user_data */) {
	if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u) {
		++g_validation_errors;
	}
	log_info("Vulkan validation (test): %s\n", data->pMessage);
	return VK_FALSE;
}

// Finds a memory type index with the wanted properties.
uint32_t find_memory_type(const VkPhysicalDevice physical_device,
                          const uint32_t type_filter,
                          const VkMemoryPropertyFlags wanted) {
	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
		if (((type_filter & (1u << i)) != 0u) &&
		    (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
			return i;
		}
	}
	throw wexception("test_vulkan: no suitable memory type");
}

// The shared headless Vulkan environment: an instance and device without a
// surface or swapchain. Created lazily on the first testcase; unavailable()
// when the machine has no usable Vulkan (the testcases then pass trivially).
struct VulkanContext {
	VulkanContext() {
		if (volkInitialize() != VK_SUCCESS) {
			log_info("test_vulkan: no Vulkan loader library; skipping the recording tests\n");
			return;
		}
		try {
			VkApplicationInfo app_info{};
			app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			app_info.pApplicationName = "widelands test_vulkan";
			app_info.apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);

			// The validation layer, when installed: the test asserts zero
			// validation errors, which is the point of the whole exercise.
			uint32_t layer_count = 0;
			vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
			std::vector<VkLayerProperties> layers(layer_count);
			vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
			const char* validation_layer = nullptr;
			for (const VkLayerProperties& layer : layers) {
				if (strncmp(layer.layerName, "VK_LAYER_KHRONOS_validation",
				            VK_MAX_EXTENSION_NAME_SIZE) == 0) {
					validation_layer = "VK_LAYER_KHRONOS_validation";
					break;
				}
			}

			const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
			VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
			messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
			                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
			messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
			messenger_info.pfnUserCallback = validation_counter;

			VkInstanceCreateInfo instance_info{};
			instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			instance_info.pApplicationInfo = &app_info;
			instance_info.enabledExtensionCount = 1;
			instance_info.ppEnabledExtensionNames = extensions;
			if (validation_layer != nullptr) {
				instance_info.enabledLayerCount = 1;
				instance_info.ppEnabledLayerNames = &validation_layer;
				instance_info.pNext = &messenger_info;
			}
			if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
				instance = VK_NULL_HANDLE;
				throw wexception("vkCreateInstance failed");
			}
			volkLoadInstance(instance);

			if (validation_layer != nullptr) {
				if (vkCreateDebugUtilsMessengerEXT(instance, &messenger_info, nullptr, &messenger) !=
				    VK_SUCCESS) {
					messenger = VK_NULL_HANDLE;
				}
			}

			uint32_t device_count = 0;
			if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS ||
			    device_count == 0) {
				throw wexception("no physical devices");
			}
			std::vector<VkPhysicalDevice> devices(device_count);
			vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
			for (const VkPhysicalDevice candidate : devices) {
				uint32_t family_count = 0;
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
				std::vector<VkQueueFamilyProperties> families(family_count);
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
				for (uint32_t family = 0; family < family_count; ++family) {
					if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
						physical_device = candidate;
						queue_family = family;
						break;
					}
				}
				if (physical_device != VK_NULL_HANDLE) {
					break;
				}
			}
			if (physical_device == VK_NULL_HANDLE) {
				throw wexception("no physical device with a graphics queue");
			}

			const float priority = 1.0f;
			VkDeviceQueueCreateInfo queue_info{};
			queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue_info.queueFamilyIndex = queue_family;
			queue_info.queueCount = 1;
			queue_info.pQueuePriorities = &priority;
			VkDeviceCreateInfo device_info{};
			device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			device_info.queueCreateInfoCount = 1;
			device_info.pQueueCreateInfos = &queue_info;
			if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
				throw wexception("vkCreateDevice failed");
			}
			volkLoadDevice(device);
			vkGetDeviceQueue(device, queue_family, 0, &queue);
			log_info("test_vulkan: recording into a headless Vulkan device\n");
		} catch (const std::exception& e) {
			log_info("test_vulkan: skipping the recording tests: %s\n", e.what());
			destroy();
		}
	}

	~VulkanContext() {
		destroy();
	}

	VulkanContext(const VulkanContext&) = delete;
	VulkanContext& operator=(const VulkanContext&) = delete;

	static VulkanContext& get() {
		static VulkanContext context;
		return context;
	}

	bool available() const {
		return device != VK_NULL_HANDLE;
	}

	void destroy() {
		if (device != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(device);
			vkDestroyDevice(device, nullptr);
			device = VK_NULL_HANDLE;
		}
		if (messenger != VK_NULL_HANDLE) {
			vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
			messenger = VK_NULL_HANDLE;
		}
		if (instance != VK_NULL_HANDLE) {
			vkDestroyInstance(instance, nullptr);
			instance = VK_NULL_HANDLE;
		}
	}

	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
};

// Builds the fill_rect pipeline from the committed SPIR-V modules embedded at
// the top of this file. Shared by TestTarget and the WP-16b offscreen
// testcases, which build it against the game-shaped LOAD render pass.
VkPipeline make_fill_rect_pipeline(const VulkanContext& context,
                                   const VkRenderPass render_pass,
                                   const VkDescriptorSetLayout set_layout) {
	const VkDevice device = context.device;
	const auto make_module = [device](const uint32_t* words, const size_t size) {
		VkShaderModuleCreateInfo module_info{};
		module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_info.codeSize = size;
		module_info.pCode = words;
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateShaderModule failed");
		}
		return module;
	};
	VkShaderModule vertex_module = make_module(kFillRectVertSpirv, sizeof(kFillRectVertSpirv));
	VkShaderModule fragment_module = make_module(kFillRectFragSpirv, sizeof(kFillRectFragSpirv));

	VkPipelineShaderStageCreateInfo stages[2] {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex_module;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment_module;
	stages[1].pName = "main";

	// The fill_rect vertex layout (bindings.json: location 0 =
	// attr_position, location 1 = attr_color; catalog stride 28).
	VkVertexInputAttributeDescription attributes[2] {};
	attributes[0].location = 0;
	attributes[0].binding = 0;
	attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributes[0].offset = offsetof(Vertex, x);
	attributes[1].location = 1;
	attributes[1].binding = 0;
	attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[1].offset = offsetof(Vertex, r);
	VkVertexInputBindingDescription binding{};
	binding.binding = 0;
	binding.stride = sizeof(Vertex);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	VkPipelineVertexInputStateCreateInfo vertex_input{};
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = 2;
	vertex_input.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo input_assembly{};
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state{};
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2;
	dynamic_state.pDynamicStates = dynamic_states;
	VkPipelineViewportStateCreateInfo viewport_state{};
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterization{};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo color_blend{};
	color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blend.attachmentCount = 1;
	color_blend.pAttachments = &blend_attachment;

	// No depth attachment in the target, so the depth test must be off.
	VkPipelineDepthStencilStateCreateInfo depth_stencil{};
	depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil.depthTestEnable = VK_FALSE;
	depth_stencil.depthWriteEnable = VK_FALSE;

	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	// A null set layout means the empty layout (fill_rect's real one); a
	// set layout passed by the descriptor-binding testcase declares
	// bindings the shader never samples, which Vulkan permits.
	if (set_layout != VK_NULL_HANDLE) {
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &set_layout;
	}
	VkPipelineLayout layout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
		throw wexception("test_vulkan: vkCreatePipelineLayout failed");
	}

	VkGraphicsPipelineCreateInfo pipeline_info{};
	pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.stageCount = 2;
	pipeline_info.pStages = stages;
	pipeline_info.pVertexInputState = &vertex_input;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pViewportState = &viewport_state;
	pipeline_info.pRasterizationState = &rasterization;
	pipeline_info.pMultisampleState = &multisampling;
	pipeline_info.pDepthStencilState = &depth_stencil;
	pipeline_info.pColorBlendState = &color_blend;
	pipeline_info.pDynamicState = &dynamic_state;
	pipeline_info.layout = layout;
	pipeline_info.renderPass = render_pass;
	pipeline_info.subpass = 0;
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result =
	   vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
	vkDestroyShaderModule(device, vertex_module, nullptr);
	vkDestroyShaderModule(device, fragment_module, nullptr);
	vkDestroyPipelineLayout(device, layout, nullptr);
	if (result != VK_SUCCESS) {
		throw wexception("test_vulkan: vkCreateGraphicsPipelines failed (%d)",
		                 static_cast<int>(result));
	}
	return pipeline;
}

// The per-testcase drawing environment: a 64x64 color target (image, view,
// render pass, framebuffer, wrapped in a Rhi::VulkanTexture), the fill_rect
// pipeline, a command buffer to record into, a staging arena, and a
// host-visible copyback buffer.
struct TestTarget {
	explicit TestTarget(VulkanContext& context) : context_(context) {
		const VkDevice device = context_.device;

		VkImageCreateInfo image_info{};
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
		image_info.extent = {kTargetSize, kTargetSize, 1};
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage =
		   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImage image = VK_NULL_HANDLE;
		if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateImage failed");
		}
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device, image, &requirements);
		VkMemoryAllocateInfo allocate_info{};
		allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize = requirements.size;
		allocate_info.memoryTypeIndex =
		   find_memory_type(context_.physical_device, requirements.memoryTypeBits,
		                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory image_memory = VK_NULL_HANDLE;
		if (vkAllocateMemory(device, &allocate_info, nullptr, &image_memory) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkAllocateMemory failed");
		}
		vkBindImageMemory(device, image, image_memory, 0);

		VkImageViewCreateInfo view_info{};
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image = image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
		view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view = VK_NULL_HANDLE;
		if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateImageView failed");
		}

		// The offscreen render pass: colour-only, clears on entry, leaves the
		// image in TRANSFER_SRC_OPTIMAL so the copyback below can read it.
		VkAttachmentDescription attachment{};
		attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkAttachmentReference color_reference{};
		color_reference.attachment = 0;
		color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color_reference;
		VkRenderPassCreateInfo render_pass_info{};
		render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		render_pass_info.attachmentCount = 1;
		render_pass_info.pAttachments = &attachment;
		render_pass_info.subpassCount = 1;
		render_pass_info.pSubpasses = &subpass;
		VkRenderPass render_pass = VK_NULL_HANDLE;
		if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateRenderPass failed");
		}

		VkFramebufferCreateInfo framebuffer_info{};
		framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer_info.renderPass = render_pass;
		framebuffer_info.attachmentCount = 1;
		framebuffer_info.pAttachments = &view;
		framebuffer_info.width = kTargetSize;
		framebuffer_info.height = kTargetSize;
		framebuffer_info.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if (vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateFramebuffer failed");
		}

		texture.reset(new Rhi::VulkanTexture(device, kTargetSize, kTargetSize, image, image_memory,
		                                view, render_pass, framebuffer));

		pipeline = create_fill_rect_pipeline(render_pass, VK_NULL_HANDLE);

		VkCommandPoolCreateInfo pool_info{};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = context_.queue_family;
		if (vkCreateCommandPool(device, &pool_info, nullptr, &command_pool) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateCommandPool failed");
		}
		VkCommandBufferAllocateInfo command_buffer_info{};
		command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		command_buffer_info.commandPool = command_pool;
		command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		command_buffer_info.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &command_buffer_info, &command_buffer) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkAllocateCommandBuffers failed");
		}
		// The game's VulkanDevice::begin_frame begins the command buffer;
		// the headless test has no frame loop, so it begins here.
		VkCommandBufferBeginInfo begin_info{};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkBeginCommandBuffer failed");
		}

		// The staging arena for the vertex buffer.
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(context_.physical_device, &properties);
		arena.reset(new Rhi::VulkanArena(device,
		                            find_memory_type(
		                               context_.physical_device, ~0u,
		                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
		                            std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 256u),
		                            1u << 20));
		arena_ptr = arena.get();
		vertex_buffer.reset(new Rhi::VulkanBuffer(arena_ptr));

		// The copyback buffer: TRANSFER_DST, host-visible coherent.
		VkBufferCreateInfo copy_buffer_info{};
		copy_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		copy_buffer_info.size = kTargetSize * kTargetSize * 4;
		copy_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		copy_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device, &copy_buffer_info, nullptr, &copy_buffer) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateBuffer failed");
		}
		VkMemoryRequirements copy_requirements{};
		vkGetBufferMemoryRequirements(device, copy_buffer, &copy_requirements);
		VkMemoryAllocateInfo copy_allocate_info{};
		copy_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		copy_allocate_info.allocationSize = copy_requirements.size;
		copy_allocate_info.memoryTypeIndex =
		   find_memory_type(context_.physical_device, copy_requirements.memoryTypeBits,
		                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(device, &copy_allocate_info, nullptr, &copy_memory) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkAllocateMemory failed");
		}
		vkBindBufferMemory(device, copy_buffer, copy_memory, 0);
	}

	~TestTarget() {
		const VkDevice device = context_.device;
		vkDestroyBuffer(device, copy_buffer, nullptr);
		vkFreeMemory(device, copy_memory, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		// texture must go before the command pool? No - independent objects;
		// any order works here as long as the device is alive.
		vkDestroyCommandPool(device, command_pool, nullptr);
		texture.reset();
	}

	// The command buffer target for Rhi::VulkanCommandBuffer (colour-only pass).
	Rhi::VulkanCommandBuffer::Target target() const {
		return Rhi::VulkanCommandBuffer::Target{
		   texture->render_pass(), texture->framebuffer(), {kTargetSize, kTargetSize}, 1};
	}

	// Submits the recorded commands, copies the target back and reads the
	// pixels (row 0 = top of the image, as in Vulkan window space).
	std::vector<uint8_t> read_back() const {
		const VkDevice device = context_.device;

		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {kTargetSize, kTargetSize, 1};
		vkCmdCopyImageToBuffer(command_buffer, texture->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       copy_buffer, 1, &region);
		vkEndCommandBuffer(command_buffer);

		VkSubmitInfo submit_info{};
		submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers = &command_buffer;
		if (vkQueueSubmit(context_.queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkQueueSubmit failed");
		}
		vkQueueWaitIdle(context_.queue);

		void* mapped = nullptr;
		vkMapMemory(device, copy_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
		std::vector<uint8_t> pixels(kTargetSize * kTargetSize * 4);
		std::memcpy(pixels.data(), mapped, pixels.size());
		vkUnmapMemory(device, copy_memory);
		return pixels;
	}

	VkPipeline create_fill_rect_pipeline(VkRenderPass render_pass,
	                                     VkDescriptorSetLayout set_layout) const {
		return make_fill_rect_pipeline(context_, render_pass, set_layout);
	}

	VulkanContext& context_;
	std::unique_ptr<Rhi::VulkanTexture> texture;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	VkBuffer copy_buffer = VK_NULL_HANDLE;
	VkDeviceMemory copy_memory = VK_NULL_HANDLE;
	std::unique_ptr<Rhi::VulkanArena> arena;
	// The buffers route through this pointer the way VulkanDevice's
	// current_arena_ works (WP-17): update() always allocates from the arena
	// the pointer names.
	Rhi::VulkanArena* arena_ptr = nullptr;
	std::unique_ptr<Rhi::VulkanBuffer> vertex_buffer;

	DISALLOW_COPY_AND_ASSIGN(TestTarget);
};

// The per-testcase upload environment for VulkanTexture::upload (WP-16):
// the VulkanUploadContext (one-shot command pool + fence, staging buffer
// grown on demand) and a linear clamp sampler.
struct UploadFixture {
	explicit UploadFixture(VulkanContext& context) : context_(context) {
		const VkDevice device = context_.device;

		VkCommandPoolCreateInfo pool_info{};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = context_.queue_family;
		if (vkCreateCommandPool(device, &pool_info, nullptr, &upload_context.command_pool) !=
		    VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateCommandPool (upload) failed");
		}
		upload_context.device = device;
		upload_context.physical_device = context_.physical_device;
		upload_context.queue = context_.queue;
		VkFenceCreateInfo fence_info{};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateFence(device, &fence_info, nullptr, &upload_context.fence) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateFence (upload) failed");
		}

		VkSamplerCreateInfo sampler_info{};
		sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		sampler_info.magFilter = VK_FILTER_LINEAR;
		sampler_info.minFilter = VK_FILTER_LINEAR;
		sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.maxLod = 0.0f;
		if (vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateSampler failed");
		}
	}

	~UploadFixture() {
		const VkDevice device = context_.device;
		vkDestroySampler(device, sampler, nullptr);
		if (upload_context.staging_buffer != VK_NULL_HANDLE) {
			vkUnmapMemory(device, upload_context.staging_memory);
			vkDestroyBuffer(device, upload_context.staging_buffer, nullptr);
			vkFreeMemory(device, upload_context.staging_memory, nullptr);
		}
		vkDestroyFence(device, upload_context.fence, nullptr);
		vkDestroyCommandPool(device, upload_context.command_pool, nullptr);
	}

	VulkanContext& context_;
	Rhi::VulkanUploadContext upload_context;
	VkSampler sampler = VK_NULL_HANDLE;

	DISALLOW_COPY_AND_ASSIGN(UploadFixture);
};

// The game-shaped offscreen render pass (WP-16b): one RGBA8 attachment,
// LOAD on entry (draws over existing contents, no clear), left
// shader-read-only, no depth. Mirrors VulkanPipelineCache::Impl's pass so
// the offscreen testcases exercise the exact attachment contract the game's
// render targets are built against.
VkRenderPass make_load_render_pass(const VkDevice device) {
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

	// The LOAD waits for the image's previous use, and the pass's writes
	// become visible to the sampling that follows - same pair of
	// dependencies as the game's pass.
	VkSubpassDependency dependencies[2] {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstAccessMask =
	   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo render_pass_info{};
	render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.attachmentCount = 1;
	render_pass_info.pAttachments = &attachment;
	render_pass_info.subpassCount = 1;
	render_pass_info.pSubpasses = &subpass;
	render_pass_info.dependencyCount = 2;
	render_pass_info.pDependencies = dependencies;
	VkRenderPass render_pass = VK_NULL_HANDLE;
	if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS) {
		throw wexception("test_vulkan: vkCreateRenderPass (load) failed");
	}
	return render_pass;
}

// A sampled texture that is also a render target, in exactly the shape the
// game produces (WP-16b): a VulkanTexture built through the sampled-texture
// constructor carrying the shared offscreen render pass, with the framebuffer
// built lazily by the command buffer's begin_pass. Plus the recording and
// copyback machinery to drive passes through it.
struct RenderTargetFixture {
	RenderTargetFixture(VulkanContext& context,
	                    const VkSampler sampler,
	                    Rhi::VulkanUploadContext* const upload)
	   : context_(context) {
		const VkDevice device = context_.device;
		render_pass = make_load_render_pass(device);
		texture.reset(new Rhi::VulkanTexture(device, kTargetSize, kTargetSize,
		                                     Rhi::TextureFormat::kRGBA8,
		                                     Rhi::TextureFilter::kLinear, sampler, upload,
		                                     render_pass));

		VkCommandPoolCreateInfo pool_info{};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = context_.queue_family;
		if (vkCreateCommandPool(device, &pool_info, nullptr, &command_pool) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateCommandPool failed");
		}

		// The copyback buffer: TRANSFER_DST, host-visible coherent.
		VkBufferCreateInfo copy_buffer_info{};
		copy_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		copy_buffer_info.size = kTargetSize * kTargetSize * 4;
		copy_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		copy_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device, &copy_buffer_info, nullptr, &copy_buffer) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkCreateBuffer (copyback) failed");
		}
		VkMemoryRequirements copy_requirements{};
		vkGetBufferMemoryRequirements(device, copy_buffer, &copy_requirements);
		VkMemoryAllocateInfo copy_allocate_info{};
		copy_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		copy_allocate_info.allocationSize = copy_requirements.size;
		copy_allocate_info.memoryTypeIndex =
		   find_memory_type(context_.physical_device, copy_requirements.memoryTypeBits,
		                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(device, &copy_allocate_info, nullptr, &copy_memory) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkAllocateMemory (copyback) failed");
		}
		vkBindBufferMemory(device, copy_buffer, copy_memory, 0);
	}

	~RenderTargetFixture() {
		const VkDevice device = context_.device;
		vkDestroyBuffer(device, copy_buffer, nullptr);
		vkFreeMemory(device, copy_memory, nullptr);
		vkDestroyCommandPool(device, command_pool, nullptr);
		texture.reset();
		vkDestroyRenderPass(device, render_pass, nullptr);
	}

	// Allocates and begins a fresh one-shot command buffer from the pool.
	VkCommandBuffer begin_commands() const {
		const VkDevice device = context_.device;
		VkCommandBufferAllocateInfo allocate_info{};
		allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate_info.commandPool = command_pool;
		allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate_info.commandBufferCount = 1;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(device, &allocate_info, &commands) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkAllocateCommandBuffers failed");
		}
		VkCommandBufferBeginInfo begin_info{};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commands, &begin_info) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkBeginCommandBuffer failed");
		}
		return commands;
	}

	// Copies the image back into the given command buffer (transitioning it
	// from the pass's shader-read-only final layout), ends the buffer,
	// submits and waits - the same pattern as TestTarget::read_back: the
	// caller passes the buffer that recorded the pass, so pass and copyback
	// submit together. Row 0 = top of the image.
	std::vector<uint8_t> read_back(VkCommandBuffer commands) const {
		const VkDevice device = context_.device;

		const VkImageSubresourceRange subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = texture->image();
		barrier.subresourceRange = subresource;
		// The pass's own 0 -> external dependency made the attachment writes
		// available to the fragment stage; this barrier chains that
		// availability through to the transfer read (the same pattern as the
		// upload test's copyback).
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &barrier);

		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {kTargetSize, kTargetSize, 1};
		vkCmdCopyImageToBuffer(commands, texture->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       copy_buffer, 1, &region);

		// Put the image back into the layout the pass left it in, so the
		// texture's tracked layout stays the truth about the image (the
		// re-upload testcase relies on it - the game's own transitions keep
		// this invariant too).
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &barrier);
		if (vkEndCommandBuffer(commands) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkEndCommandBuffer (copyback) failed");
		}

		VkSubmitInfo submit_info{};
		submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.commandBufferCount = 1;
		submit_info.pCommandBuffers = &commands;
		if (vkQueueSubmit(context_.queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
			throw wexception("test_vulkan: vkQueueSubmit (copyback) failed");
		}
		vkQueueWaitIdle(context_.queue);

		void* mapped = nullptr;
		vkMapMemory(device, copy_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
		std::vector<uint8_t> pixels(kTargetSize * kTargetSize * 4);
		std::memcpy(pixels.data(), mapped, pixels.size());
		vkUnmapMemory(device, copy_memory);
		return pixels;
	}

	VulkanContext& context_;
	VkRenderPass render_pass = VK_NULL_HANDLE;
	std::unique_ptr<Rhi::VulkanTexture> texture;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkBuffer copy_buffer = VK_NULL_HANDLE;
	VkDeviceMemory copy_memory = VK_NULL_HANDLE;

	DISALLOW_COPY_AND_ASSIGN(RenderTargetFixture);
};

// Appends one quad (two triangles) in canonical clip space at depth 0.5 with
// a uniform colour. The fill_rect shader bakes z into the vertex.
void append_quad(std::vector<Vertex>& vertices,                 const float x0,
                 const float y0,
                 const float x1,
                 const float y1,
                 const float r,
                 const float g,
                 const float b) {
	const Vertex bottom_left{x0, y0, 0.5f, r, g, b, 1.f};
	const Vertex bottom_right{x1, y0, 0.5f, r, g, b, 1.f};
	const Vertex top_right{x1, y1, 0.5f, r, g, b, 1.f};
	const Vertex top_left{x0, y1, 0.5f, r, g, b, 1.f};
	vertices.push_back(bottom_left);
	vertices.push_back(bottom_right);
	vertices.push_back(top_right);
	vertices.push_back(bottom_left);
	vertices.push_back(top_right);
	vertices.push_back(top_left);
}

// One pixel in the copyback (B, G, R, A byte order).
struct Pixel {
	uint8_t b;
	uint8_t g;
	uint8_t r;
	uint8_t a;
};

const Pixel& pixel_at(const std::vector<uint8_t>& pixels, const int x, const int y) {
	return *reinterpret_cast<const Pixel*>(pixels.data() + (y * kTargetSize + x) * 4);
}

// Asserts that the pixel matches the expected RGBA within a tolerance of 2
// (unorm rounding).
void check_pixel(const std::vector<uint8_t>& pixels,
                 const int x,
                 const int y,
                 const int r,
                 const int g,
                 const int b,
                 const int a = 255) {
	const Pixel& pixel = pixel_at(pixels, x, y);
	WLTestsuite::do_check_equal(__FILE__, __LINE__, std::abs(static_cast<int>(pixel.r) - r) <= 2, true);
	WLTestsuite::do_check_equal(__FILE__, __LINE__, std::abs(static_cast<int>(pixel.g) - g) <= 2, true);
	WLTestsuite::do_check_equal(__FILE__, __LINE__, std::abs(static_cast<int>(pixel.b) - b) <= 2, true);
	WLTestsuite::do_check_equal(__FILE__, __LINE__, std::abs(static_cast<int>(pixel.a) - a) <= 2, true);
}

// The same check for a copyback in R,G,B,A byte order (the R8G8B8A8 textures
// the WP-16b fixtures use; check_pixel above reads B,G,R,A, the byte order of
// TestTarget's B8G8R8A8 image).
void check_rgba_pixel(const std::vector<uint8_t>& pixels,
                      const int x,
                      const int y,
                      const int r,
                      const int g,
                      const int b,
                      const int a = 255) {
	const uint8_t* pixel = pixels.data() + (static_cast<size_t>(y) * kTargetSize + x) * 4;
	WLTestsuite::do_check_equal(
	   __FILE__, __LINE__, std::abs(static_cast<int>(pixel[0]) - r) <= 2, true);
	WLTestsuite::do_check_equal(
	   __FILE__, __LINE__, std::abs(static_cast<int>(pixel[1]) - g) <= 2, true);
	WLTestsuite::do_check_equal(
	   __FILE__, __LINE__, std::abs(static_cast<int>(pixel[2]) - b) <= 2, true);
	WLTestsuite::do_check_equal(
	   __FILE__, __LINE__, std::abs(static_cast<int>(pixel[3]) - a) <= 2, true);
}

// The clear colour the testcases clear the target to.
constexpr float kClearR = 0.1f;
constexpr float kClearG = 0.2f;
constexpr float kClearB = 0.3f;

int clear_r() {
	return static_cast<int>(kClearR * 255.f);
}
int clear_g() {
	return static_cast<int>(kClearG * 255.f);
}
int clear_b() {
	return static_cast<int>(kClearB * 255.f);
}

}  // namespace

TESTSUITE_START(vulkan_command_buffer)

/*
 * The whole recording path on one frame: clear, a viewport into the
 * canonical bottom-left quadrant (which exercises the SPIR-V Y-flip + the
 * flipped viewport compensation) and one arena-uploaded quad drawn with the
 * fill_rect pipeline. Drawing a pipeline that requires descriptor bindings
 * without a bound set is an error (WP-16), pinned separately below.
 */
TESTCASE(records_a_flipped_viewport_draw) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}
	const int errors_before = g_validation_errors;

	TestTarget target(context);
	Rhi::VulkanPipeline pipeline("fill_rect", Rhi::kBlendOpaque, /* requires_binding */ false,
	                        target.pipeline);

	Rhi::VulkanCommandBuffer command_buffer(
	   context.device, target.command_buffer, nullptr, target.target());
	command_buffer.begin_pass(target.texture.get(),
	                          Rhi::PassClear{true, kClearR, kClearG, kClearB, 1.f});
	command_buffer.set_viewport(Recti(16, 16, 32, 32));

	std::vector<Vertex> vertices;
	append_quad(vertices, -1.f, -1.f, 1.f, 1.f, 1.f, 0.f, 0.f);
	target.vertex_buffer->update(vertices.data(), vertices.size() * sizeof(Vertex));
	command_buffer.bind_pipeline(&pipeline);
	command_buffer.bind_vertex_buffer(target.vertex_buffer.get());
	command_buffer.draw(0, vertices.size());

	command_buffer.end_pass();
	const std::vector<uint8_t> pixels = target.read_back();

	// The viewport rect (16,16)-(48,48) in canonical bottom-left coordinates
	// is rows 16..47 from the top in the copyback.
	check_pixel(pixels, 8, 8, clear_r(), clear_g(), clear_b());
	check_pixel(pixels, 16, 16, 255, 0, 0);
	check_pixel(pixels, 32, 32, 255, 0, 0);
	check_pixel(pixels, 47, 47, 255, 0, 0);
	check_pixel(pixels, 48, 48, clear_r(), clear_g(), clear_b());
	check_pixel(pixels, 63, 63, clear_r(), clear_g(), clear_b());

	check_equal(g_validation_errors, errors_before);
}

/*
 * Scissor clipping: a full-screen draw, then a scissored draw over the
 * canonical bottom-left block, then a third draw after disable_scissor. Pins
 * the scissor flip and the enable/disable round trip.
 */
TESTCASE(scissor_clips_to_the_flipped_rectangle) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}
	const int errors_before = g_validation_errors;

	TestTarget target(context);
	Rhi::VulkanPipeline pipeline("fill_rect", Rhi::kBlendOpaque, /* requires_binding */ false,
	                        target.pipeline);

	Rhi::VulkanCommandBuffer command_buffer(
	   context.device, target.command_buffer, nullptr, target.target());
	command_buffer.begin_pass(target.texture.get(),
	                          Rhi::PassClear{true, kClearR, kClearG, kClearB, 1.f});
	command_buffer.set_viewport(Recti(0, 0, kTargetSize, kTargetSize));

	std::vector<Vertex> vertices;
	append_quad(vertices, -1.f, -1.f, 1.f, 1.f, 0.f, 1.f, 0.f);
	target.vertex_buffer->update(vertices.data(), vertices.size() * sizeof(Vertex));
	command_buffer.bind_pipeline(&pipeline);
	command_buffer.bind_vertex_buffer(target.vertex_buffer.get());
	command_buffer.draw(0, vertices.size());

	// Red quad clipped to the canonical bottom-left block.
	command_buffer.set_scissor(Recti(16, 16, 32, 32));
	vertices.clear();
	append_quad(vertices, -1.f, -1.f, 1.f, 1.f, 1.f, 0.f, 0.f);
	target.vertex_buffer->update(vertices.data(), vertices.size() * sizeof(Vertex));
	command_buffer.bind_vertex_buffer(target.vertex_buffer.get());
	command_buffer.draw(0, vertices.size());

	// Blue quad in the canonical top-right quarter, after the scissor is off
	// again: proves disable_scissor restores the full extent.
	command_buffer.disable_scissor();
	vertices.clear();
	append_quad(vertices, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f);
	target.vertex_buffer->update(vertices.data(), vertices.size() * sizeof(Vertex));
	command_buffer.bind_vertex_buffer(target.vertex_buffer.get());
	command_buffer.draw(0, vertices.size());

	command_buffer.end_pass();
	const std::vector<uint8_t> pixels = target.read_back();

	// Canonical bottom-left block: top-origin rows 16..47.
	check_pixel(pixels, 32, 32, 255, 0, 0);
	// Outside the scissor, outside the blue quad: still green.
	check_pixel(pixels, 8, 8, 0, 255, 0);
	// The blue quad: canonical top-right quarter, top-origin rows 0..31,
	// columns 32..63.
	check_pixel(pixels, 56, 16, 0, 0, 255);

	check_equal(g_validation_errors, errors_before);
}

/*
 * draw(vertex_offset, vertex_count): two quads uploaded in one arena region,
 * only the second drawn.
 */
TESTCASE(draw_offset_selects_vertices) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}
	const int errors_before = g_validation_errors;

	TestTarget target(context);
	Rhi::VulkanPipeline pipeline("fill_rect", Rhi::kBlendOpaque, /* requires_binding */ false,
	                        target.pipeline);

	Rhi::VulkanCommandBuffer command_buffer(
	   context.device, target.command_buffer, nullptr, target.target());
	command_buffer.begin_pass(target.texture.get(),
	                          Rhi::PassClear{true, kClearR, kClearG, kClearB, 1.f});
	command_buffer.set_viewport(Recti(0, 0, kTargetSize, kTargetSize));

	// Quad 1 (red): canonical bottom-left quarter. Quad 2 (green): canonical
	// top-right quarter.
	std::vector<Vertex> vertices;
	append_quad(vertices, -1.f, -1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
	append_quad(vertices, 0.f, 0.f, 1.f, 1.f, 0.f, 1.f, 0.f);
	target.vertex_buffer->update(vertices.data(), vertices.size() * sizeof(Vertex));
	command_buffer.bind_pipeline(&pipeline);
	command_buffer.bind_vertex_buffer(target.vertex_buffer.get());
	command_buffer.draw(6, 6);

	command_buffer.end_pass();
	const std::vector<uint8_t> pixels = target.read_back();

	// The green quad: canonical top-right quarter = top-origin rows 0..31,
	// columns 32..63.
	check_pixel(pixels, 48, 16, 0, 255, 0);
	// The red quad's region was not drawn.
	check_pixel(pixels, 16, 48, clear_r(), clear_g(), clear_b());

	check_equal(g_validation_errors, errors_before);
}

/*
 * The staging arena: transient regions (fresh offset per update), alignment,
 * copied bytes, and the reset that rewinds allocations.
 */
TESTCASE(arena_allocates_fresh_aligned_transient_regions) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(context.physical_device, &properties);
	const uint32_t alignment =
	   std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 256u);
	Rhi::VulkanArena arena(context.device,
	                  find_memory_type(context.physical_device, ~0u,
	                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
	                  alignment, 1u << 20);
	Rhi::VulkanArena* current_arena = &arena;
	Rhi::VulkanBuffer buffer(current_arena);

	std::vector<uint8_t> data(1000, 0xabu);
	// Measure from a reset state: offsets are chunk-relative, and the
	// constructor's initial chunk fill would make the first update land in a
	// second chunk.
	arena.reset();
	buffer.update(data.data(), data.size());
	const VkDeviceSize first_offset = buffer.offset();
	check_equal(first_offset % alignment, static_cast<VkDeviceSize>(0));
	check_equal(static_cast<bool>(buffer.buffer() != VK_NULL_HANDLE), true);

	// A second update within the same frame gets a fresh, non-overlapping
	// region (transient semantics).
	buffer.update(data.data(), data.size());
	check_equal(buffer.offset() >= first_offset + data.size(), true);
	check_equal(buffer.offset() % alignment, static_cast<VkDeviceSize>(0));

	// reset() rewinds: the next allocation reuses the first offset.
	arena.reset();
	buffer.update(data.data(), data.size());
	check_equal(buffer.offset(), first_offset);

	// The copied bytes are what the caller passed.
	const Rhi::VulkanArena::Region region = arena.allocate(64);
	std::vector<uint8_t> pattern(64, 0x5au);
	std::memcpy(region.mapped, pattern.data(), pattern.size());
	check_equal(std::memcmp(region.mapped, pattern.data(), pattern.size()), 0);
	arena.reset();
}

/*
 * The texture upload path (WP-16): VulkanTexture::upload stages the pixels,
 * transitions the image to shader-read-only and finishes before returning.
 * The copyback asserts the image holds the uploaded bytes in the same row
 * order (row 0 = the first uploaded row = shader v = 0), which is the
 * canonical-orientation contract rhi.h pins for both backends.
 */
TESTCASE(texture_upload_is_byte_exact_and_row_ordered) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}
	const int errors_before = g_validation_errors;

	constexpr uint32_t kTexW = 8;
	constexpr uint32_t kTexH = 4;
	UploadFixture fixture(context);
	Rhi::VulkanTexture texture(context.device, kTexW, kTexH, Rhi::TextureFormat::kRGBA8,
	                          Rhi::TextureFilter::kLinear, fixture.sampler,
	                          &fixture.upload_context);

	// Each row carries its own row index in the red channel, so a wrong row
	// order is visible immediately.
	std::vector<uint8_t> pixels(static_cast<size_t>(kTexW) * kTexH * 4);
	for (uint32_t y = 0; y < kTexH; ++y) {
		for (uint32_t x = 0; x < kTexW; ++x) {
			const size_t i = (static_cast<size_t>(y) * kTexW + x) * 4;
			pixels[i + 0] = static_cast<uint8_t>(y);
			pixels[i + 1] = static_cast<uint8_t>(x);
			pixels[i + 2] = 128;
			pixels[i + 3] = 255;
		}
	}
	texture.upload(pixels.data());

	// Copy the image back through a one-shot command buffer.
	const VkDevice device = context.device;
	const VkDeviceSize copy_size = pixels.size();
	VkBufferCreateInfo copy_buffer_info{};
	copy_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	copy_buffer_info.size = copy_size;
	copy_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	copy_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer copy_buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(device, &copy_buffer_info, nullptr, &copy_buffer) != VK_SUCCESS) {
		throw wexception("test_vulkan: vkCreateBuffer (copyback) failed");
	}
	VkMemoryRequirements copy_requirements{};
	vkGetBufferMemoryRequirements(device, copy_buffer, &copy_requirements);
	VkMemoryAllocateInfo copy_allocate_info{};
	copy_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	copy_allocate_info.allocationSize = copy_requirements.size;
	copy_allocate_info.memoryTypeIndex =
	   find_memory_type(context.physical_device, copy_requirements.memoryTypeBits,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory copy_memory = VK_NULL_HANDLE;
	if (vkAllocateMemory(device, &copy_allocate_info, nullptr, &copy_memory) != VK_SUCCESS) {
		throw wexception("test_vulkan: vkAllocateMemory (copyback) failed");
	}
	vkBindBufferMemory(device, copy_buffer, copy_memory, 0);

	VkCommandBufferAllocateInfo allocate_info{};
	allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate_info.commandPool = fixture.upload_context.command_pool;
	allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate_info.commandBufferCount = 1;
	VkCommandBuffer copy_commands = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(device, &allocate_info, &copy_commands) != VK_SUCCESS) {
		throw wexception("test_vulkan: vkAllocateCommandBuffers (copyback) failed");
	}
	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(copy_commands, &begin_info);

	const VkImageSubresourceRange subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = texture.image();
	barrier.subresourceRange = subresource;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(copy_commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	VkBufferImageCopy region{};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {kTexW, kTexH, 1};
	vkCmdCopyImageToBuffer(copy_commands, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       copy_buffer, 1, &region);

	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(copy_commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &barrier);
	vkEndCommandBuffer(copy_commands);

	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &copy_commands;
	// The upload above left the shared fence signaled; a submit needs it
	// unsignaled.
	vkResetFences(device, 1, &fixture.upload_context.fence);
	if (vkQueueSubmit(context.queue, 1, &submit_info, fixture.upload_context.fence) != VK_SUCCESS) {
		throw wexception("test_vulkan: vkQueueSubmit (copyback) failed");
	}
	vkWaitForFences(device, 1, &fixture.upload_context.fence, VK_TRUE, UINT64_MAX);

	void* mapped = nullptr;
	vkMapMemory(device, copy_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	check_equal(std::memcmp(mapped, pixels.data(), pixels.size()), 0);
	vkUnmapMemory(device, copy_memory);

	vkDestroyBuffer(device, copy_buffer, nullptr);
	vkFreeMemory(device, copy_memory, nullptr);
	vkFreeCommandBuffers(device, fixture.upload_context.command_pool, 1, &copy_commands);

	check_equal(g_validation_errors, errors_before);
}

/*
 * The descriptor binding path (WP-16): a descriptor set whose layout
 * declares one sampler (binding 0) and one uniform buffer (binding 1, after
 * the sampler in the manifest's shared counter - the terrain-like
 * translation). Drawing without a bound set throws; with the set bound the
 * draw records and the descriptors pass validation.
 */
TESTCASE(descriptor_set_binding_allocates_writes_and_binds) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}
	const int errors_before = g_validation_errors;

	TestTarget target(context);
	UploadFixture fixture(context);

	// A 4x4 texture (content is irrelevant - the fill_rect shader never
	// samples; the descriptor write must still be valid).
	std::vector<uint8_t> texels(4u * 4 * 4, 0x7fu);
	Rhi::VulkanTexture texture(context.device, 4, 4, Rhi::TextureFormat::kRGBA8,
	                          Rhi::TextureFilter::kLinear, fixture.sampler,
	                          &fixture.upload_context);
	texture.upload(texels.data());

	// The descriptor set layout: sampler at binding 0 (fragment stage), a
	// uniform buffer at binding 1 (vertex stage) - bindings the shader does
	// not use, but the pipeline layout may declare.
	VkDescriptorSetLayoutBinding layout_bindings[2] {};
	layout_bindings[0].binding = 0;
	layout_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layout_bindings[0].descriptorCount = 1;
	layout_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layout_bindings[1].binding = 1;
	layout_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	layout_bindings[1].descriptorCount = 1;
	layout_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	VkDescriptorSetLayoutCreateInfo set_layout_info{};
	set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = 2;
	set_layout_info.pBindings = layout_bindings;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	if (vkCreateDescriptorSetLayout(context.device, &set_layout_info, nullptr, &set_layout) !=
	    VK_SUCCESS) {
		throw wexception("test_vulkan: vkCreateDescriptorSetLayout failed");
	}
	VkPipelineLayoutCreateInfo pipeline_layout_info{};
	pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts = &set_layout;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(context.device, &pipeline_layout_info, nullptr, &pipeline_layout) !=
	    VK_SUCCESS) {
		throw wexception("test_vulkan: vkCreatePipelineLayout failed");
	}

	const VkPipeline fill_rect_with_bindings =
	   target.create_fill_rect_pipeline(target.texture->render_pass(), set_layout);
	Rhi::VulkanPipeline pipeline("fill_rect", Rhi::kBlendOpaque, /* requires_binding */ true,
	                        fill_rect_with_bindings);

	// The manifest entry the set translates through: sampler index 0 at
	// Vulkan binding 0, uniform buffer index 0 at Vulkan binding 1.
	Rhi::ManifestProgram manifest{};
	manifest.samplers.emplace_back("u_test", 0u);
	manifest.uniform_blocks.push_back({"test_block", 1u, true, false});

	Rhi::VulkanDescriptorSet descriptor_set(
	   "fill_rect", &manifest, set_layout, pipeline_layout, /* requires_binding */ true);

	// The per-frame descriptor pool (the device owns one; the headless test
	// creates its own for the command buffer to allocate from).
	VkDescriptorPoolSize pool_sizes[2] {};
	pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_sizes[0].descriptorCount = 8;
	pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_sizes[1].descriptorCount = 8;
	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 8;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = pool_sizes;
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	if (vkCreateDescriptorPool(context.device, &pool_info, nullptr, &descriptor_pool) !=
	    VK_SUCCESS) {
		throw wexception("test_vulkan: vkCreateDescriptorPool failed");
	}

	Rhi::VulkanCommandBuffer command_buffer(
	   context.device, target.command_buffer, nullptr, target.target(), descriptor_pool, nullptr);
	command_buffer.begin_pass(target.texture.get(),
	                          Rhi::PassClear{true, kClearR, kClearG, kClearB, 1.f});
	command_buffer.set_viewport(Recti(0, 0, kTargetSize, kTargetSize));

	// A quad and a uniform buffer region (256 bytes, the arena alignment).
	std::vector<Vertex> vertices;
	append_quad(vertices, -1.f, -1.f, 1.f, 1.f, 0.f, 0.f, 1.f);
	target.vertex_buffer->update(vertices.data(), vertices.size() * sizeof(Vertex));

	command_buffer.bind_pipeline(&pipeline);
	// Drawing without a bound descriptor set is an error, not a silent skip.
	check_error(WException, "requires a bound descriptor set",
	            [&command_buffer]() { command_buffer.draw(0, 6); });

	descriptor_set.set_texture(0, &texture);
	descriptor_set.set_uniform_buffer(0, target.vertex_buffer.get(), 0, 256);
	command_buffer.bind_descriptor_set(&descriptor_set);
	command_buffer.bind_vertex_buffer(target.vertex_buffer.get());
	command_buffer.draw(0, vertices.size());

	command_buffer.end_pass();
	const std::vector<uint8_t> pixels = target.read_back();

	// The blue quad covered the whole viewport.
	check_pixel(pixels, 8, 8, 0, 0, 255);
	check_pixel(pixels, 32, 32, 0, 0, 255);
	check_pixel(pixels, 63, 63, 0, 0, 255);

	check_equal(g_validation_errors, errors_before);

	vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
	vkDestroyPipeline(context.device, fill_rect_with_bindings, nullptr);
	vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
	vkDestroyDescriptorSetLayout(context.device, set_layout, nullptr);
}

/*
 * The immediate render-to-texture path (WP-16b): a sampled texture that is
 * also a render target, drawn into exactly as the game's Texture::draw_to_self
 * does - transition to colour-attachment, a pass that loads (not clears),
 * the full-extent viewport recorded by begin_pass itself, a draw into the
 * canonical bottom-left quarter, end_pass, then a redundant transition back
 * to shader-read-only (a no-op: the pass's final layout is already that).
 * Pins the lazy framebuffer, the load semantics, the layout bookkeeping and
 * the transition no-op, all with zero validation errors.
 */
TESTCASE(offscreen_pass_loads_preserves_and_transitions) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}
	const int errors_before = g_validation_errors;

	UploadFixture upload_fixture(context);
	RenderTargetFixture target(context, upload_fixture.sampler, &upload_fixture.upload_context);

	// Pre-fill through the upload path: the left half blue, the right half
	// green. The pass must preserve everything the red quad does not cover.
	std::vector<uint8_t> pixels(kTargetSize * kTargetSize * 4);
	for (uint32_t y = 0; y < kTargetSize; ++y) {
		for (uint32_t x = 0; x < kTargetSize; ++x) {
			const size_t i = (static_cast<size_t>(y) * kTargetSize + x) * 4;
			if (x < kTargetSize / 2) {
				pixels[i + 0] = 0;    // r
				pixels[i + 1] = 0;    // g
				pixels[i + 2] = 255;  // b
			} else {
				pixels[i + 0] = 0;    // r
				pixels[i + 1] = 255;  // g
				pixels[i + 2] = 0;    // b
			}
			pixels[i + 3] = 255;
		}
	}
	target.texture->upload(pixels.data());
	check_equal(target.texture->current_layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);

	// The framebuffer does not exist until the first pass into the texture.
	check_equal(target.texture->framebuffer() == VK_NULL_HANDLE, true);

	VkPipeline pipeline_handle =
	   make_fill_rect_pipeline(context, target.render_pass, VK_NULL_HANDLE);
	Rhi::VulkanPipeline pipeline("fill_rect", Rhi::kBlendOpaque, /* requires_binding */ false,
	                             pipeline_handle);

	// The game's begin_offscreen passes an empty screen target: an offscreen
	// command buffer never draws to the swapchain.
	const VkCommandBuffer commands = target.begin_commands();
	Rhi::VulkanCommandBuffer command_buffer(
	   context.device, commands, nullptr, Rhi::VulkanCommandBuffer::Target{});

	command_buffer.transition(target.texture.get(), Rhi::TextureLayout::kColorAttachment);
	check_equal(target.texture->current_layout() == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
	command_buffer.begin_pass(target.texture.get(), Rhi::PassClear{false, 0.f, 0.f, 0.f, 0.f});
	check_equal(target.texture->framebuffer() != VK_NULL_HANDLE, true);

	// A red quad in the canonical bottom-left quarter; begin_pass recorded
	// the full-extent viewport itself (the game's texture.cc relies on it).
	std::vector<Vertex> vertices;
	append_quad(vertices, -1.f, -1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(context.physical_device, &properties);
	Rhi::VulkanArena arena(
	   context.device,
	   find_memory_type(context.physical_device, ~0u,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
	   std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 256u), 1u << 20);
	Rhi::VulkanArena* current_arena = &arena;
	Rhi::VulkanBuffer vertex_buffer(current_arena);
	vertex_buffer.update(vertices.data(), vertices.size() * sizeof(Vertex));
	command_buffer.bind_pipeline(&pipeline);
	command_buffer.bind_vertex_buffer(&vertex_buffer);
	command_buffer.draw(0, vertices.size());

	command_buffer.end_pass();
	// The pass's final layout is shader-read-only; the caller's post-pass
	// transition to the same layout records nothing.
	check_equal(target.texture->current_layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
	command_buffer.transition(target.texture.get(), Rhi::TextureLayout::kShaderReadOnly);
	check_equal(target.texture->current_layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);

	const std::vector<uint8_t> result = target.read_back(commands);

	// The canonical bottom-left quarter: top-origin rows 32..63, columns
	// 0..31, red.
	check_rgba_pixel(result, 16, 48, 255, 0, 0);
	check_rgba_pixel(result, 31, 63, 255, 0, 0);
	// Outside the quad: the left half stays blue, the right half green.
	check_rgba_pixel(result, 16, 16, 0, 0, 255);
	check_rgba_pixel(result, 48, 16, 0, 255, 0);
	check_rgba_pixel(result, 48, 48, 0, 255, 0);

	check_equal(g_validation_errors, errors_before);
	vkDestroyPipeline(context.device, pipeline_handle, nullptr);
}

/*
 * The minimap flow (WP-16b): after the offscreen pass, the texture is
 * re-uploaded through unlock(Unlock_Update). upload() must transition from
 * the layout the texture is actually in (shader-read-only after the pass) -
 * a hard UNDEFINED oldLayout would trip the validation layer - and the new
 * pixels must land byte-exactly.
 */
TESTCASE(reupload_after_render_transitions_from_the_current_layout) {
	VulkanContext& context = VulkanContext::get();
	if (!context.available()) {
		return;
	}
	const int errors_before = g_validation_errors;

	UploadFixture upload_fixture(context);
	RenderTargetFixture target(context, upload_fixture.sampler, &upload_fixture.upload_context);

	std::vector<uint8_t> first_pixels(kTargetSize * kTargetSize * 4, 42);
	first_pixels[3] = 255;
	target.texture->upload(first_pixels.data());

	VkPipeline pipeline_handle =
	   make_fill_rect_pipeline(context, target.render_pass, VK_NULL_HANDLE);
	Rhi::VulkanPipeline pipeline("fill_rect", Rhi::kBlendOpaque, /* requires_binding */ false,
	                             pipeline_handle);

	const VkCommandBuffer commands = target.begin_commands();
	Rhi::VulkanCommandBuffer command_buffer(
	   context.device, commands, nullptr, Rhi::VulkanCommandBuffer::Target{});
	command_buffer.transition(target.texture.get(), Rhi::TextureLayout::kColorAttachment);
	command_buffer.begin_pass(target.texture.get(), Rhi::PassClear{false, 0.f, 0.f, 0.f, 0.f});

	std::vector<Vertex> vertices;
	append_quad(vertices, -1.f, -1.f, 1.f, 1.f, 0.f, 0.f, 1.f);
	Rhi::VulkanArena arena(
	   context.device,
	   find_memory_type(context.physical_device, ~0u,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
	   256u, 1u << 20);
	Rhi::VulkanArena* current_arena = &arena;
	Rhi::VulkanBuffer vertex_buffer(current_arena);
	vertex_buffer.update(vertices.data(), vertices.size() * sizeof(Vertex));
	command_buffer.bind_pipeline(&pipeline);
	command_buffer.bind_vertex_buffer(&vertex_buffer);
	command_buffer.draw(0, vertices.size());
	command_buffer.end_pass();

	// Submit the pass (the copyback rides along in its own buffer) and
	// verify the pass's own output first.
	const std::vector<uint8_t> drawn = target.read_back(commands);
	check_rgba_pixel(drawn, 8, 8, 0, 0, 255);
	check_equal(target.texture->current_layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);

	// Now the minimap's re-upload: new CPU content pushed into the texture
	// that was just rendered into. Each row carries its own row index in the
	// red channel, so both the layout transition and the row order are
	// pinned.
	std::vector<uint8_t> new_pixels(kTargetSize * kTargetSize * 4);
	for (uint32_t y = 0; y < kTargetSize; ++y) {
		for (uint32_t x = 0; x < kTargetSize; ++x) {
			const size_t i = (static_cast<size_t>(y) * kTargetSize + x) * 4;
			new_pixels[i + 0] = static_cast<uint8_t>(y);
			new_pixels[i + 1] = static_cast<uint8_t>(x);
			new_pixels[i + 2] = 128;
			new_pixels[i + 3] = 255;
		}
	}
	target.texture->upload(new_pixels.data());
	check_equal(target.texture->current_layout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);

	const std::vector<uint8_t> result = target.read_back(target.begin_commands());
	check_equal(result.size(), new_pixels.size());
	check_equal(std::memcmp(result.data(), new_pixels.data(), new_pixels.size()), 0);

	check_equal(g_validation_errors, errors_before);
	vkDestroyPipeline(context.device, pipeline_handle, nullptr);
}

TESTSUITE_END()
