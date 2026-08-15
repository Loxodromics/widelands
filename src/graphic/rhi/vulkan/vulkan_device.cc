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
#include <vector>

#include <volk.h>

#include <SDL_vulkan.h>

#include "base/log.h"

namespace Rhi {

namespace {

// The placeholder fill colour presented until real drawing lands (WP-14
// onwards). Deliberately not black: a dark purple is unmistakably the
// bootstrap clear, so a present that silently stops drawing is distinguishable
// from an unrendered black window.
const VkClearColorValue kClearColor = {{0.15f, 0.05f, 0.20f, 1.0f}};

// Validation layers are a debug-build feature (plan WP-12 scope): release
// builds pay nothing and never depend on the layer being installed.
#ifdef NDEBUG
constexpr bool kEnableValidation = false;
#else
constexpr bool kEnableValidation = true;
#endif

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

// One image-layout barrier, with the queue families fixed to ignored (the
// queue family is a single graphics/present one, so no ownership transfer is
// ever expressed).
void image_barrier(const VkCommandBuffer command_buffer,
                   const VkImage image,
                   const VkImageLayout old_layout,
                   const VkImageLayout new_layout,
                   const VkAccessFlags src_access,
                   const VkAccessFlags dst_access,
                   const VkPipelineStageFlags src_stage,
                   const VkPipelineStageFlags dst_stage) {
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = src_access;
	barrier.dstAccessMask = dst_access;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
	                     &barrier);
}

}  // namespace

// All Vulkan handles and state live here so vulkan_device.h stays free of
// Vulkan types (and the pimpl is the reason <memory> is in the header).
struct VulkanDevice::Impl {
	explicit Impl(SDL_Window* sdl_window);
	~Impl();

	// Creates (or re-creates after a resize) the swapchain and the per-frame
	// clear command buffer. Returns false if the surface is currently
	// unusable (e.g. minimized to a zero-size extent); present() skips the
	// frame then.
	bool recreate_swapchain();

	void present();

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

	if (!recreate_swapchain()) {
		throw wexception("Vulkan: the window has no drawable size.");
	}
}

VulkanDevice::Impl::~Impl() {
	vkDeviceWaitIdle(device);
	vkDestroyFence(device, frame_fence, nullptr);
	vkDestroyFence(device, acquire_fence, nullptr);
	vkDestroyCommandPool(device, command_pool, nullptr);
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

	// The clear happens through vkCmdClearColorImage, so the images need
	// TRANSFER_DST on top of the mandatory COLOR_ATTACHMENT usage. Every
	// driver on the target hardware advertises this combination.
	constexpr VkImageUsageFlags kSwapchainUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((capabilities.supportedUsageFlags & kSwapchainUsage) != kSwapchainUsage) {
		throw wexception("Vulkan: the surface does not support TRANSFER_DST image usage, which "
		                 "the bootstrap clear needs.");
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

	// One frame in flight with a blocking fence wait, so the old swapchain is
	// never referenced when it is destroyed. WP-17 owns real frame
	// pipelining; the device-wide stall before destroying the old swapchain is
	// a bootstrap-grade correctness measure (a pending present may still
	// reference the images) and goes away with proper per-frame
	// synchronization.
	vkDeviceWaitIdle(device);
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

	verb_log_info("Graphics: Vulkan: Swapchain: %dx%d (format %u, %u images)\n", extent.width,
	              extent.height, static_cast<unsigned>(image_format), image_count);
	return true;
}

void VulkanDevice::Impl::present() {
	// Fully serialized bootstrap: one fence, no semaphores at all. The fence
	// wait before the acquire guarantees the queue is done with every image;
	// the acquire itself guarantees the presentation engine is done with the
	// image it hands out (and, with a NULL semaphore, blocks until then); the
	// fence wait after the submit guarantees the clear is visible before the
	// present is queued. Semaphores only buy parallelism, which WP-17 adds
	// with real frames in flight - and skipping them here removes the whole
	// class of pending-semaphore validation errors that any partially
	// synchronized loop trips.
	check_vulkan_result(vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX),
	                    "vkWaitForFences");

	check_vulkan_result(vkResetFences(device, 1, &acquire_fence), "vkResetFences");
	uint32_t image_index = 0;
	const VkResult acquire_result = vkAcquireNextImageKHR(
	   device, swapchain, UINT64_MAX, VK_NULL_HANDLE, acquire_fence, &image_index);
	if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR || acquire_result == VK_SUBOPTIMAL_KHR) {
		// Window was resized; the swapchain no longer matches the surface.
		// Recreate and skip this frame. The fence is left signaled, so the
		// next frame's wait passes immediately.
		recreate_swapchain();
		return;
	}
	if (acquire_result != VK_SUCCESS) {
		log_warn("Vulkan: skipping a frame: vkAcquireNextImageKHR: %s\n",
		         vulkan_result_string(acquire_result));
		return;
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

	const VkImage image = swapchain_images[image_index];
	image_barrier(command_buffer, image, VK_IMAGE_LAYOUT_UNDEFINED,
	              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	const VkImageSubresourceRange clear_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &kClearColor,
	                     1, &clear_range);

	image_barrier(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
	              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	check_vulkan_result(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");

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
	present_info.pImageIndices = &image_index;
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
}

VulkanDevice::~VulkanDevice() = default;

void VulkanDevice::present() {
	impl_->present();
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

void VulkanDevice::present() {
}

}  // namespace Rhi

#endif  // WL_BUILD_VULKAN
