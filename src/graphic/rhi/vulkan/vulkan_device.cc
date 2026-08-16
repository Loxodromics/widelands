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

#include "graphic/rhi/vulkan/vulkan_device.h"

#include "base/wexception.h"

#ifdef WL_BUILD_VULKAN

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <volk.h>

#include <SDL_vulkan.h>

#include "base/log.h"
#include "graphic/rhi/device.h"
#include "graphic/rhi/vulkan/vulkan_buffer.h"
#include "graphic/rhi/vulkan/vulkan_command_buffer.h"
#include "graphic/rhi/vulkan/vulkan_pipeline_cache.h"
#include "graphic/rhi/vulkan/vulkan_resources.h"

namespace Rhi {

namespace {

// Validation layers are a debug-build feature (plan WP-12 scope): release
// builds pay nothing and never depend on the layer being installed.
#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

// The initial staging-arena size (WP-15): one full frame's vertex traffic is
// well below this even on dense scenes; the arena grows by allocating more
// chunks when a frame exceeds it.
constexpr VkDeviceSize kInitialArenaSize = 32u * 1024u * 1024u;

// Names the VkResult codes the bootstrap can surface; anything else is
// reported numerically. A hand-rolled subset rather than the full
// vulkan_to_string.hpp, which drags in the complete C++ Vulkan bindings.
const char* vulkan_result_string(const VkResult result) {
	switch (result) {
	case VK_SUCCESS:
		return "VK_SUCCESS";
	case VK_NOT_READY:
		return "VK_NOT_READY";
	case VK_TIMEOUT:
		return "VK_TIMEOUT";
	case VK_SUBOPTIMAL_KHR:
		return "VK_SUBOPTIMAL_KHR";
	case VK_ERROR_OUT_OF_HOST_MEMORY:
		return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:
		return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED:
		return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST:
		return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_LAYER_NOT_PRESENT:
		return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT:
		return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER:
		return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_OUT_OF_DATE_KHR:
		return "VK_ERROR_OUT_OF_DATE_KHR";
	case VK_ERROR_SURFACE_LOST_KHR:
		return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
		return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
	default:
		return "unknown VkResult";
	}
}

// Throws on a failed VkResult with a human-readable result name.
void check_vulkan_result(const VkResult result, const char* what) {
	if (result != VK_SUCCESS) {
		throw wexception("Vulkan %s failed: %s (%d)", what, vulkan_result_string(result),
		                 static_cast<int>(result));
	}
}

// Debug-utils messenger callback (debug builds only): routes validation
// errors and warnings into the log. Only ERROR/WARNING severities are
// subscribed, so the loader's informational chatter stays out of the log and
// "validation layers silent" is checkable by grepping the run log.
VkBool32 VKAPI_CALL vulkan_debug_callback(
   const VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
   VkDebugUtilsMessageTypeFlagsEXT /* message_type */,
   const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
   void* /* user_data */) {
	if ((message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u) {
		log_err("Vulkan validation: %s\n", callback_data->pMessage);
	} else {
		log_warn("Vulkan validation: %s\n", callback_data->pMessage);
	}
	return VK_FALSE;
}

// Picks the depth attachment format for the screen render pass: prefer a
// 32-bit float depth, fall back through the common 24/16-bit formats. Every
// driver the plan targets (section 3 of RENDERER_MODERNIZATION_PLAN.md)
// supports at least one of these.
VkFormat choose_depth_format(const VkPhysicalDevice physical_device) {
	for (const VkFormat candidate :
	     {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM}) {
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(physical_device, candidate, &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
		    0u) {
			return candidate;
		}
	}
	throw wexception("Vulkan: no supported depth attachment format found.");
}

// Finds a memory type index satisfying 'type_filter' with 'properties'. The
// renderer allocates one image per swapchain recreation, so no allocator is
// warranted; this is the plain vkAllocateMemory path.
uint32_t find_memory_type(const VkPhysicalDevice physical_device,
                          const uint32_t type_filter,
                          const VkMemoryPropertyFlags wanted_properties) {
	VkPhysicalDeviceMemoryProperties memory_properties{};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
		if (((type_filter & (1u << i)) != 0u) &&
		    (memory_properties.memoryTypes[i].propertyFlags & wanted_properties) ==
		       wanted_properties) {
			return i;
		}
	}
	throw wexception("Vulkan: no suitable memory type found.");
}

// The memory type for the staging arena: prefer device-local host-visible
// coherent memory (BAR memory on the primary box), fall back to plain
// host-visible coherent. Every driver the plan targets offers at least one
// of the two.
uint32_t find_arena_memory_type(const VkPhysicalDevice physical_device) {
	VkPhysicalDeviceMemoryProperties memory_properties{};
	vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
	uint32_t fallback = UINT32_MAX;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
		const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
		if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			continue;
		}
		if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) {
			return i;
		}
		if (fallback == UINT32_MAX) {
			fallback = i;
		}
	}
	if (fallback != UINT32_MAX) {
		return fallback;
	}
	throw wexception("Vulkan: no host-visible coherent memory type for the staging arena.");
}

}  // namespace

// All Vulkan handles and state live here so vulkan_device.h stays free of
// Vulkan types (and the pimpl is the reason <memory> is in the header).
struct VulkanDevice::Impl {
	explicit Impl(SDL_Window* sdl_window);
	~Impl();

	// Creates (or re-creates after a resize) the swapchain, the depth
	// attachment and the per-frame framebuffers. Returns false if the surface
	// is currently unusable (e.g. minimized to a zero-size extent); the
	// frame loop skips the frame then.
	bool recreate_swapchain();

	// The frame loop (WP-15): acquire, record, submit, present. begin_frame
	// returns a VulkanCommandBuffer bound to the acquired framebuffer, or a
	// no-op buffer when the frame is dropped (resize, lost surface).
	// end_frame finishes the recording and presents.
	std::unique_ptr<CommandBuffer> begin_frame();
	void end_frame(std::unique_ptr<CommandBuffer> command_buffer);

	// Creates the depth image, its memory and view, plus one framebuffer per
	// swapchain image (colour view + the shared depth view). 'pipeline' must
	// be the render pass these framebuffers target.
	void create_depth_and_framebuffers(VkRenderPass render_pass);

	SDL_Window* window = nullptr;

	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat image_format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent{};
	std::vector<VkImage> swapchain_images;

	// The screen render pass machinery (renderer modernization plan, WP-14):
	// the depth attachment backing the render pass, one framebuffer per
	// swapchain image (each with its own colour view - a framebuffer does not
	// retain image views, they must stay alive with it), and the pipeline
	// cache (render pass + the twelve pipelines) rebuilt whenever the
	// swapchain format changes.
	VkFormat depth_format = VK_FORMAT_UNDEFINED;
	VkImage depth_image = VK_NULL_HANDLE;
	VkDeviceMemory depth_memory = VK_NULL_HANDLE;
	VkImageView depth_view = VK_NULL_HANDLE;
	std::vector<VkImageView> color_views;
	std::vector<VkFramebuffer> framebuffers;
	std::unique_ptr<VulkanPipelineCache> pipelines;

	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	// The submit fence (queue is done with the last frame) and a second fence
	// for the acquire: vkAcquireNextImageKHR requires at least one of its
	// semaphore/fence parameters to be non-null, and without the fence the
	// validation layer flags every frame (VUID-vkAcquireNextImageKHR-
	// semaphore-01780). Nobody waits on the acquire fence - the blocking
	// acquire already synchronizes - it is reset before each acquire purely
	// so it can be re-signaled.
	VkFence frame_fence = VK_NULL_HANDLE;
	VkFence acquire_fence = VK_NULL_HANDLE;

	// The per-frame staging arena (WP-15): every vertex and uniform update
	// allocates a fresh region here; reset after the previous frame's fence
	// wait in begin_frame.
	std::unique_ptr<VulkanArena> arena;

	// Set when begin_frame dropped the frame (swapchain recreation); end_frame
	// then submits nothing.
	bool frame_dropped_ = false;

	// The swapchain image index the current frame acquired and end_frame must
	// present.
	uint32_t frame_image_index_ = 0;
};

VulkanDevice::Impl::Impl(SDL_Window* sdl_window) : window(sdl_window) {
	if (volkInitialize() != VK_SUCCESS) {
		throw wexception("Failed to load the Vulkan loader library (libvulkan.so.1).");
	}

	// The extensions SDL needs for the surface (VK_KHR_surface plus the
	// platform surface extension; SDL lists whichever apply). VK_EXT_debug_utils
	// is ours, for the validation messenger below.
	uint32_t sdl_extension_count = 0;
	if (SDL_Vulkan_GetInstanceExtensions(window, &sdl_extension_count, nullptr) != SDL_TRUE) {
		throw wexception("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
	}
	std::vector<const char*> instance_extensions(sdl_extension_count);
	if (SDL_Vulkan_GetInstanceExtensions(
	       window, &sdl_extension_count, instance_extensions.data()) != SDL_TRUE) {
		throw wexception("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
	}
	if (kEnableValidation) {
		instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	// Enable the validation layer when present (it should be on the primary
	// box); a missing layer is warned about rather than fatal, so a debug
	// build still runs on machines without it.
	const char* validation_layer = nullptr;
	if (kEnableValidation) {
		uint32_t layer_count = 0;
		vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
		std::vector<VkLayerProperties> layers(layer_count);
		vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
		for (const VkLayerProperties& layer : layers) {
			if (strncmp(layer.layerName, "VK_LAYER_KHRONOS_validation", VK_MAX_EXTENSION_NAME_SIZE) ==
			    0) {
				validation_layer = "VK_LAYER_KHRONOS_validation";
				break;
			}
		}
		if (validation_layer == nullptr) {
			log_warn("Vulkan: VK_LAYER_KHRONOS_validation not installed; running without "
			         "validation\n");
		}
	}

	VkApplicationInfo application_info{};
	application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application_info.pApplicationName = "Widelands";
	application_info.applicationVersion = 1;
	application_info.pEngineName = "Widelands";
	application_info.apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);

	VkInstanceCreateInfo instance_create_info{};
	instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_create_info.pApplicationInfo = &application_info;
	instance_create_info.enabledExtensionCount = instance_extensions.size();
	instance_create_info.ppEnabledExtensionNames = instance_extensions.data();
	if (validation_layer != nullptr) {
		instance_create_info.enabledLayerCount = 1;
		instance_create_info.ppEnabledLayerNames = &validation_layer;
	}
	check_vulkan_result(vkCreateInstance(&instance_create_info, nullptr, &instance),
	                    "vkCreateInstance");
	volkLoadInstance(instance);

	if (validation_layer != nullptr) {
		VkDebugUtilsMessengerCreateInfoEXT messenger_create_info{};
		messenger_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		messenger_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
		                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
		messenger_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messenger_create_info.pfnUserCallback = vulkan_debug_callback;
		check_vulkan_result(vkCreateDebugUtilsMessengerEXT(
		                       instance, &messenger_create_info, nullptr, &debug_messenger),
		                    "vkCreateDebugUtilsMessengerEXT");
	}

	if (SDL_Vulkan_CreateSurface(window, instance, &surface) != SDL_TRUE) {
		throw wexception("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
	}

	// Pick the first physical device with a queue family that does both
	// graphics and present. Every driver the plan targets (section 3) has a
	// combined family; a split family is deliberately unsupported at the
	// bootstrap stage.
	uint32_t device_count = 0;
	check_vulkan_result(vkEnumeratePhysicalDevices(instance, &device_count, nullptr),
	                    "vkEnumeratePhysicalDevices");
	if (device_count == 0) {
		throw wexception("Vulkan: no physical devices found.");
	}
	std::vector<VkPhysicalDevice> devices(device_count);
	check_vulkan_result(vkEnumeratePhysicalDevices(instance, &device_count, devices.data()),
	                    "vkEnumeratePhysicalDevices");
	for (const VkPhysicalDevice candidate : devices) {
		uint32_t family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
		for (uint32_t family_index = 0; family_index < family_count; ++family_index) {
			VkBool32 present_support = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family_index, surface, &present_support);
			if ((families[family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u &&
			    present_support == VK_TRUE) {
				physical_device = candidate;
				queue_family = family_index;
				break;
			}
		}
		if (physical_device != VK_NULL_HANDLE) {
			break;
		}
	}
	if (physical_device == VK_NULL_HANDLE) {
		throw wexception(
		   "Vulkan: no physical device with a graphics queue that can present to the window.");
	}

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physical_device, &properties);
	log_info("Graphics: Vulkan: Device: \"%s\" (type %u, driver %u, api %u.%u.%u)\n",
	         properties.deviceName, static_cast<unsigned>(properties.deviceType),
	         properties.driverVersion, VK_API_VERSION_MAJOR(properties.apiVersion),
	         VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));

	depth_format = choose_depth_format(physical_device);
	log_info("Graphics: Vulkan: Depth attachment format: %u\n",
	         static_cast<unsigned>(depth_format));

	const float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_create_info{};
	queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_create_info.queueFamilyIndex = queue_family;
	queue_create_info.queueCount = 1;
	queue_create_info.pQueuePriorities = &queue_priority;

	// VK_KHR_swapchain is a *device* extension (unlike VK_KHR_surface, which
	// is instance-level): the loader only hands out the swapchain entry
	// points through vkGetDeviceProcAddr when the device was created with it
	// enabled, and without them volk leaves the function pointers null - a
	// jump to zero at vkCreateSwapchainKHR. Check support and enable it.
	{
		uint32_t extension_count = 0;
		check_vulkan_result(vkEnumerateDeviceExtensionProperties(
		                       physical_device, nullptr, &extension_count, nullptr),
		                    "vkEnumerateDeviceExtensionProperties");
		std::vector<VkExtensionProperties> extensions(extension_count);
		check_vulkan_result(vkEnumerateDeviceExtensionProperties(
		                       physical_device, nullptr, &extension_count, extensions.data()),
		                    "vkEnumerateDeviceExtensionProperties");
		bool swapchain_supported = false;
		for (const VkExtensionProperties& extension : extensions) {
			if (strncmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			            VK_MAX_EXTENSION_NAME_SIZE) == 0) {
				swapchain_supported = true;
				break;
			}
		}
		if (!swapchain_supported) {
			throw wexception("Vulkan: the physical device does not support VK_KHR_swapchain.");
		}
	}

	const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkDeviceCreateInfo device_create_info{};
	device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_create_info.queueCreateInfoCount = 1;
	device_create_info.pQueueCreateInfos = &queue_create_info;
	device_create_info.enabledExtensionCount = 1;
	device_create_info.ppEnabledExtensionNames = device_extensions;
	check_vulkan_result(vkCreateDevice(physical_device, &device_create_info, nullptr, &device),
	                    "vkCreateDevice");
	volkLoadDevice(device);
	vkGetDeviceQueue(device, queue_family, 0, &queue);

	VkCommandPoolCreateInfo command_pool_create_info{};
	command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
	                                 VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	command_pool_create_info.queueFamilyIndex = queue_family;
	check_vulkan_result(
	   vkCreateCommandPool(device, &command_pool_create_info, nullptr, &command_pool),
	   "vkCreateCommandPool");
	VkCommandBufferAllocateInfo command_buffer_allocate_info{};
	command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_buffer_allocate_info.commandPool = command_pool;
	command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_buffer_allocate_info.commandBufferCount = 1;
	check_vulkan_result(
	   vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer),
	   "vkAllocateCommandBuffers");

	// Signaled at creation so the first frame's fence wait passes; afterwards
	// the fence is reset per frame and signaled by the per-frame submit.
	VkFenceCreateInfo fence_create_info{};
	fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	check_vulkan_result(vkCreateFence(device, &fence_create_info, nullptr, &frame_fence),
	                    "vkCreateFence");
	fence_create_info.flags = 0;
	check_vulkan_result(vkCreateFence(device, &fence_create_info, nullptr, &acquire_fence),
	                    "vkCreateFence");

	// The staging arena (WP-15): allocations are aligned to the device's
	// uniform-buffer-offset alignment so WP-16 can bind any region as a UBO.
	const uint32_t arena_alignment = static_cast<uint32_t>(
	   std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 256u));
	arena.reset(new VulkanArena(
	   device, find_arena_memory_type(physical_device), arena_alignment, kInitialArenaSize));

	if (!recreate_swapchain()) {
		throw wexception("Vulkan: the window has no drawable size.");
	}
}

VulkanDevice::Impl::~Impl() {
	vkDeviceWaitIdle(device);
	// The arena must go after the idle wait (recorded draws reference its
	// regions) and before the device.
	arena.reset();
	vkDestroyFence(device, frame_fence, nullptr);
	vkDestroyFence(device, acquire_fence, nullptr);
	vkDestroyCommandPool(device, command_pool, nullptr);
	for (VkFramebuffer framebuffer : framebuffers) {
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	}
	framebuffers.clear();
	for (VkImageView color_view : color_views) {
		vkDestroyImageView(device, color_view, nullptr);
	}
	color_views.clear();
	if (depth_view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, depth_view, nullptr);
	}
	if (depth_image != VK_NULL_HANDLE) {
		vkDestroyImage(device, depth_image, nullptr);
	}
	if (depth_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, depth_memory, nullptr);
	}
	// The pipeline cache owns render-pass-bound resources and must die before
	// the swapchain-independent Vulkan objects (and the device).
	pipelines.reset();
	vkDestroySwapchainKHR(device, swapchain, nullptr);
	if (debug_messenger != VK_NULL_HANDLE) {
		vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, nullptr);
	}
	vkDestroySurfaceKHR(instance, surface, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
}

bool VulkanDevice::Impl::recreate_swapchain() {
	VkSurfaceCapabilitiesKHR capabilities{};
	check_vulkan_result(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	                       physical_device, surface, &capabilities),
	                    "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

	// The extent is 0 when the window is minimized (skip frames) and
	// UINT32_MAX on platforms that do not define it (ask SDL for the real
	// drawable size).
	extent = capabilities.currentExtent;
	if (extent.width == 0 || extent.height == 0) {
		return false;
	}
	if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
		int drawable_w = 0;
		int drawable_h = 0;
		SDL_Vulkan_GetDrawableSize(window, &drawable_w, &drawable_h);
		extent.width = std::max(static_cast<uint32_t>(drawable_w), 1u);
		extent.height = std::max(static_cast<uint32_t>(drawable_h), 1u);
	}
	extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
	                          capabilities.maxImageExtent.width);
	extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
	                           capabilities.maxImageExtent.height);

	uint32_t format_count = 0;
	check_vulkan_result(vkGetPhysicalDeviceSurfaceFormatsKHR(
	                       physical_device, surface, &format_count, nullptr),
	                    "vkGetPhysicalDeviceSurfaceFormatsKHR");
	if (format_count == 0) {
		throw wexception("Vulkan: the surface offers no image formats.");
	}
	std::vector<VkSurfaceFormatKHR> formats(format_count);
	check_vulkan_result(vkGetPhysicalDeviceSurfaceFormatsKHR(
	                       physical_device, surface, &format_count, formats.data()),
	                    "vkGetPhysicalDeviceSurfaceFormatsKHR");

	// Prefer the classic non-sRGB 8-bit BGRA; fall back to the first format
	// the surface offers (which is the only guaranteed one).
	VkSurfaceFormatKHR surface_format = formats[0];
	for (const VkSurfaceFormatKHR& format : formats) {
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
		    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			surface_format = format;
			break;
		}
	}
	image_format = surface_format.format;

	// FIFO (vsync) is mandatory in every implementation; more flexible modes
	// can wait for WP-17.
	const VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

	// The frame renders through the screen render pass (WP-14), so the images
	// need only the mandatory COLOR_ATTACHMENT usage.
	constexpr VkImageUsageFlags kSwapchainUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((capabilities.supportedUsageFlags & kSwapchainUsage) != kSwapchainUsage) {
		throw wexception("Vulkan: the surface does not support COLOR_ATTACHMENT image usage.");
	}

	// One frame in flight with a blocking fence wait, so the old swapchain is
	// never referenced when it is destroyed. WP-17 owns real frame
	// pipelining; the device-wide stall before destroying the old swapchain is
	// a bootstrap-grade correctness measure (a pending present may still
	// reference the images) and goes away with proper per-frame
	// synchronization.
	vkDeviceWaitIdle(device);

	// The framebuffers reference the swapchain image views, the depth view and
	// the render pass, so they must go before the old swapchain - and before
	// the render pass, when the format change below rebuilds it. The depth
	// attachment is recreated too (it is sized to the swapchain extent).
	for (VkFramebuffer framebuffer : framebuffers) {
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	}
	framebuffers.clear();
	for (VkImageView color_view : color_views) {
		vkDestroyImageView(device, color_view, nullptr);
	}
	color_views.clear();
	if (depth_view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, depth_view, nullptr);
		depth_view = VK_NULL_HANDLE;
	}
	if (depth_image != VK_NULL_HANDLE) {
		vkDestroyImage(device, depth_image, nullptr);
		depth_image = VK_NULL_HANDLE;
	}
	if (depth_memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, depth_memory, nullptr);
		depth_memory = VK_NULL_HANDLE;
	}

	// Pipelines bake in the render pass, and the render pass bakes in the
	// swapchain image format. A format change is the one reason to rebuild
	// the cache; a plain resize keeps it (viewports are dynamic state).
	const bool format_changed = image_format != surface_format.format;
	image_format = surface_format.format;
	if (format_changed) {
		pipelines.reset();
	}
	if (pipelines == nullptr) {
		pipelines.reset(new VulkanPipelineCache(device, image_format, depth_format));
	}

	VkSwapchainCreateInfoKHR swapchain_create_info{};
	swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchain_create_info.surface = surface;
	swapchain_create_info.minImageCount = capabilities.minImageCount;
	swapchain_create_info.imageFormat = surface_format.format;
	swapchain_create_info.imageColorSpace = surface_format.colorSpace;
	swapchain_create_info.imageExtent = extent;
	swapchain_create_info.imageArrayLayers = 1;
	swapchain_create_info.imageUsage = kSwapchainUsage;
	swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchain_create_info.preTransform = capabilities.currentTransform;
	swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchain_create_info.presentMode = present_mode;
	swapchain_create_info.clipped = VK_TRUE;

	VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
	check_vulkan_result(
	   vkCreateSwapchainKHR(device, &swapchain_create_info, nullptr, &new_swapchain),
	   "vkCreateSwapchainKHR");
	if (swapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(device, swapchain, nullptr);
	}
	swapchain = new_swapchain;

	uint32_t image_count = 0;
	check_vulkan_result(vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr),
	                    "vkGetSwapchainImagesKHR");
	swapchain_images.resize(image_count);
	check_vulkan_result(vkGetSwapchainImagesKHR(
	                       device, swapchain, &image_count, swapchain_images.data()),
	                    "vkGetSwapchainImagesKHR");

	create_depth_and_framebuffers(pipelines->render_pass());

	verb_log_info("Graphics: Vulkan: Swapchain: %dx%d (format %u, %u images)\n", extent.width,
	              extent.height, static_cast<unsigned>(image_format), image_count);
	return true;
}

void VulkanDevice::Impl::create_depth_and_framebuffers(const VkRenderPass render_pass) {
	// The depth attachment: one image sized to the swapchain extent, shared by
	// all framebuffers (depth is cleared every frame, so no per-image
	// isolation is needed - and there is only one frame in flight).
	VkImageCreateInfo image_create_info{};
	image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.format = depth_format;
	image_create_info.extent = {extent.width, extent.height, 1};
	image_create_info.mipLevels = 1;
	image_create_info.arrayLayers = 1;
	image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_create_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	check_vulkan_result(vkCreateImage(device, &image_create_info, nullptr, &depth_image),
	                    "vkCreateImage");

	VkMemoryRequirements memory_requirements{};
	vkGetImageMemoryRequirements(device, depth_image, &memory_requirements);
	VkMemoryAllocateInfo allocate_info{};
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = memory_requirements.size;
	allocate_info.memoryTypeIndex =
	   find_memory_type(physical_device, memory_requirements.memoryTypeBits,
	                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check_vulkan_result(vkAllocateMemory(device, &allocate_info, nullptr, &depth_memory),
	                    "vkAllocateMemory");
	check_vulkan_result(vkBindImageMemory(device, depth_image, depth_memory, 0),
	                    "vkBindImageMemory");

	VkImageViewCreateInfo view_create_info{};
	view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_create_info.image = depth_image;
	view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_create_info.format = depth_format;
	view_create_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
	check_vulkan_result(vkCreateImageView(device, &view_create_info, nullptr, &depth_view),
	                    "vkCreateImageView");

	// One framebuffer per swapchain image: its colour view plus the shared
	// depth view. The colour view is kept (framebuffers do not retain image
	// views) and destroyed with the framebuffers on the next recreation.
	VkImageView attachments[2] = {VK_NULL_HANDLE, depth_view};
	for (const VkImage image : swapchain_images) {
		VkImageViewCreateInfo color_view_create_info{};
		color_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		color_view_create_info.image = image;
		color_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		color_view_create_info.format = image_format;
		color_view_create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView color_view = VK_NULL_HANDLE;
		check_vulkan_result(
		   vkCreateImageView(device, &color_view_create_info, nullptr, &color_view),
		   "vkCreateImageView");
		color_views.push_back(color_view);
		attachments[0] = color_view;

		VkFramebufferCreateInfo framebuffer_create_info{};
		framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer_create_info.renderPass = render_pass;
		framebuffer_create_info.attachmentCount = 2;
		framebuffer_create_info.pAttachments = attachments;
		framebuffer_create_info.width = extent.width;
		framebuffer_create_info.height = extent.height;
		framebuffer_create_info.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		check_vulkan_result(
		   vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer),
		   "vkCreateFramebuffer");
		framebuffers.push_back(framebuffer);
	}
}

std::unique_ptr<CommandBuffer> VulkanDevice::Impl::begin_frame() {
	// Fully serialized bootstrap: one fence, no semaphores at all. The fence
	// wait before the acquire guarantees the queue is done with every image
	// (and with every arena region the previous frame recorded); the acquire
	// itself guarantees the presentation engine is done with the image it
	// hands out (and, with a NULL semaphore, blocks until then). Semaphores
	// only buy parallelism, which WP-17 adds with real frames in flight.
	check_vulkan_result(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX),
	                    "vkWaitForFences");
	arena->reset();

	check_vulkan_result(vkResetFences(device, 1, &acquire_fence), "vkResetFences");
	uint32_t image_index = 0;
	const VkResult acquire_result = vkAcquireNextImageKHR(
	   device, swapchain, UINT64_MAX, VK_NULL_HANDLE, acquire_fence, &image_index);
	if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR || acquire_result == VK_SUBOPTIMAL_KHR) {
		// Window was resized; the swapchain no longer matches the surface.
		// Recreate and drop this frame. The fence is left signaled, so the
		// next frame's wait passes immediately.
		recreate_swapchain();
		frame_dropped_ = true;
		return std::unique_ptr<CommandBuffer>(new VulkanNoOpCommandBuffer());
	}
	if (acquire_result != VK_SUCCESS) {
		log_warn("Vulkan: skipping a frame: vkAcquireNextImageKHR: %s\n",
		         vulkan_result_string(acquire_result));
		frame_dropped_ = true;
		return std::unique_ptr<CommandBuffer>(new VulkanNoOpCommandBuffer());
	}
	// The spec requires waiting on the acquire fence before touching the
	// image (VUID-vkAcquireNextImageKHR-fence-...): the presentation engine
	// signals it once it has finished reading the image.
	check_vulkan_result(vkWaitForFences(device, 1, &acquire_fence, VK_TRUE, UINT64_MAX),
	                    "vkWaitForFences");
	check_vulkan_result(vkResetFences(device, 1, &frame_fence), "vkResetFences");

	check_vulkan_result(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer");
	VkCommandBufferBeginInfo command_buffer_begin_info{};
	command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vulkan_result(vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info),
	                    "vkBeginCommandBuffer");

	frame_dropped_ = false;
	frame_image_index_ = image_index;
	const VulkanCommandBuffer::Target target{
	   pipelines->render_pass(), framebuffers[image_index], extent, 2};
	return std::unique_ptr<CommandBuffer>(
	   new VulkanCommandBuffer(command_buffer, pipelines.get(), target));
}

void VulkanDevice::Impl::end_frame(std::unique_ptr<CommandBuffer> command_buffer_wrapper) {
	if (frame_dropped_) {
		frame_dropped_ = false;
		return;
	}
	auto* recorded = static_cast<VulkanCommandBuffer*>(command_buffer_wrapper.get());
	recorded->finish();

	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffer;
	check_vulkan_result(vkQueueSubmit(queue, 1, &submit_info, frame_fence), "vkQueueSubmit");
	check_vulkan_result(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX),
	                    "vkWaitForFences");

	VkPresentInfoKHR present_info{};
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &swapchain;
	present_info.pImageIndices = &frame_image_index_;
	const VkResult present_result = vkQueuePresentKHR(queue, &present_info);
	if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
		recreate_swapchain();
		return;
	}
	if (present_result != VK_SUCCESS) {
		log_warn("Vulkan: skipping a frame: vkQueuePresentKHR: %s\n",
		         vulkan_result_string(present_result));
	}
}

VulkanDevice::VulkanDevice(SDL_Window* window) : impl_(std::make_unique<Impl>(window)) {
	set_device(this);
}

VulkanDevice::~VulkanDevice() {
	set_device(nullptr);
}

Backend VulkanDevice::backend() const {
	return Backend::kVulkan;
}

std::unique_ptr<CommandBuffer> VulkanDevice::begin_frame() {
	std::unique_ptr<CommandBuffer> command_buffer = impl_->begin_frame();
	push_command_buffer(command_buffer.get());
	return command_buffer;
}

void VulkanDevice::end_frame(std::unique_ptr<CommandBuffer> command_buffer) {
	impl_->end_frame(std::move(command_buffer));
	pop_command_buffer();
}

std::unique_ptr<CommandBuffer> VulkanDevice::begin_offscreen() {
	// WP-16b owns the real immediate render-to-texture path. Until then the
	// callers (font cache, image cache, minimap) record into a no-op buffer:
	// their textures stay blank, which is invisible while no descriptor binds
	// a texture (WP-16). The game keeps running either way.
	static bool warned = false;
	if (!warned) {
		warned = true;
		log_warn("Vulkan: immediate render-to-texture is a no-op until WP-16b (font, minimap "
		         "and image-cache textures stay blank)\n");
	}
	std::unique_ptr<CommandBuffer> command_buffer(new VulkanNoOpCommandBuffer());
	push_command_buffer(command_buffer.get());
	return command_buffer;
}

void VulkanDevice::submit_offscreen(std::unique_ptr<CommandBuffer> /* command_buffer */) {
	// Nothing was recorded into the no-op buffer; popping the stack restores
	// whatever command buffer was recording before this offscreen submit.
	pop_command_buffer();
}

void VulkanDevice::read_back_swapchain(uint8_t* /* pixels */) {
	throw wexception("Rhi::VulkanDevice::read_back_swapchain: swapchain readback is WP-18");
}

std::unique_ptr<Texture> VulkanDevice::create_texture(const TextureDescriptor& /* desc */) {
	throw wexception("Rhi::VulkanDevice::create_texture: texture creation and upload are WP-16");
}

std::unique_ptr<Texture>
VulkanDevice::create_texture_view(Texture& /* parent */, const Recti& /* subrect */) {
	throw wexception("Rhi::VulkanDevice::create_texture_view: sub-textures are modelled by BlitData");
}

std::unique_ptr<Buffer> VulkanDevice::create_buffer(const uint32_t /* size */,
                                                    const BufferUsage /* usage */) {
	// Both vertex and uniform data live in the per-frame staging arena; the
	// capacity hint is ignored (see VulkanBuffer).
	return std::unique_ptr<Buffer>(new VulkanBuffer(*impl_->arena));
}

std::unique_ptr<Pipeline> VulkanDevice::create_pipeline(const PipelineDescriptor& desc) {
	const bool requires_binding = impl_->pipelines->has_descriptor_bindings(desc.program_name);
	return std::unique_ptr<Pipeline>(
	   new VulkanPipeline(desc.program_name, desc.blend, requires_binding));
}

std::unique_ptr<DescriptorSet> VulkanDevice::create_descriptor_set(const Pipeline& pipeline) {
	const VulkanPipeline& vulkan_pipeline = static_cast<const VulkanPipeline&>(pipeline);
	return std::unique_ptr<DescriptorSet>(
	   new VulkanDescriptorSet(vulkan_pipeline.requires_binding()));
}

}  // namespace Rhi

#else  // WL_BUILD_VULKAN

// Builds without Vulkan support reject --renderer=vulkan at parse time
// (wlapplication.cc), so none of this is ever called; the stub exists so the
// translation unit still compiles and links without the Vulkan headers, which
// keeps the library's callers independent of the OPTION_BUILD_VULKAN choice.

namespace Rhi {

struct VulkanDevice::Impl {};

VulkanDevice::VulkanDevice(SDL_Window* /* window */) : impl_(std::make_unique<Impl>()) {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

VulkanDevice::~VulkanDevice() = default;

Backend VulkanDevice::backend() const {
	return Backend::kVulkan;
}

std::unique_ptr<CommandBuffer> VulkanDevice::begin_frame() {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

void VulkanDevice::end_frame(std::unique_ptr<CommandBuffer> /* command_buffer */) {
}

std::unique_ptr<CommandBuffer> VulkanDevice::begin_offscreen() {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

void VulkanDevice::submit_offscreen(std::unique_ptr<CommandBuffer> /* command_buffer */) {
}

void VulkanDevice::read_back_swapchain(uint8_t* /* pixels */) {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

std::unique_ptr<Texture> VulkanDevice::create_texture(const TextureDescriptor& /* desc */) {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

std::unique_ptr<Texture>
VulkanDevice::create_texture_view(Texture& /* parent */, const Recti& /* subrect */) {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

std::unique_ptr<Buffer> VulkanDevice::create_buffer(const uint32_t /* size */,
                                                    const BufferUsage /* usage */) {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

std::unique_ptr<Pipeline> VulkanDevice::create_pipeline(const PipelineDescriptor& /* desc */) {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

std::unique_ptr<DescriptorSet>
VulkanDevice::create_descriptor_set(const Pipeline& /* pipeline */) {
	throw wexception("This build has no Vulkan support (built without OPTION_BUILD_VULKAN).");
}

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
