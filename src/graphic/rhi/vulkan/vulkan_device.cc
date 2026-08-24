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
#include <array>
#include <cstring>
#include <iterator>
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

// The number of frames in flight (WP-17). Two gives the CPU one frame of
// headroom over the GPU (recording frame N+1 while frame N renders) and
// keeps the eager per-slot arena memory at 2x32 MB; the swapchain requests
// kFramesInFlight + 1 images so the acquire never blocks under FIFO.
constexpr uint32_t kFramesInFlight = 2;

// VK_KHR_portability_subset has no declaration in <volk.h>/<vulkan_core.h>:
// it is still a provisional KHR "beta" extension, only exposed by defining
// VK_ENABLE_BETA_EXTENSIONS before including the Vulkan headers. That macro
// pulls in every other beta extension's declarations too (video encode/
// decode and friends), and the vendored volk.h assumes a newer set of those
// than the Vulkan SDK headers on this machine provide, which fails to
// compile. We only need the extension's name to enable it at device
// creation - MoltenVK is the only driver that ever advertises it - so we
// spell the name out locally instead of pulling in the rest of the beta
// surface.
constexpr const char* kPortabilitySubsetExtensionName = "VK_KHR_portability_subset";

// The floor the renderer needs from maxImageDimension2D - the same number as
// graphic.h's kMinimumSizeForTextures (the atlas builder throws below it).
// Duplicated locally so the RHI library does not depend on the graphic layer.
constexpr uint32_t kMinimumVulkanTextureSize = 2048;

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
	// frame loop skips the frame then. [[nodiscard]] because every call site
	// has to decide what "still unusable" means for it - silently ignoring
	// it reads as "handled" when it was not (D5).
	[[nodiscard]] bool recreate_swapchain();

	// The frame loop (WP-15): acquire, record, submit, present. begin_frame
	// returns a VulkanCommandBuffer bound to the acquired framebuffer, or a
	// no-op buffer when the frame is dropped (resize, lost surface).
	// end_frame finishes the recording and presents.
	std::unique_ptr<CommandBuffer> begin_frame();
	void end_frame(std::unique_ptr<CommandBuffer> command_buffer);

	// WP-17 deferred texture destruction: a texture destroyed while frames
	// are in flight may still be referenced by a recorded descriptor set.
	// retire_texture_resources stamps the image/memory/view with the current
	// frame counter; begin_frame frees entries whose stamp is
	// kFramesInFlight frames old (the fence wait there has completed every
	// frame that could reference them).
	struct PendingTextureFree {
		VkImage image;
		VkDeviceMemory memory;
		VkImageView view;
		uint64_t frame_stamp;
	};
	void retire_texture_resources(VkImage image, VkDeviceMemory memory, VkImageView view);
	void drain_pending_texture_frees(bool free_all);

	// Creates one depth image, its memory and view per swapchain image, plus
	// one framebuffer per swapchain image (colour view + that image's depth
	// view). 'pipeline' must be the render pass these framebuffers target.
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
	// one depth attachment per swapchain image (WP-17: the screen pass
	// clears depth, so overlapping frames must not share one), one
	// framebuffer per swapchain image (each with its own colour and depth
	// views - a framebuffer does not retain image views, they must stay
	// alive with it), and the pipeline cache (render pass + the twelve
	// pipelines) rebuilt whenever the swapchain format changes.
	VkFormat depth_format = VK_FORMAT_UNDEFINED;
	std::vector<VkImage> depth_images;
	std::vector<VkDeviceMemory> depth_memories;
	std::vector<VkImageView> depth_views;
	std::vector<VkImageView> color_views;
	std::vector<VkFramebuffer> framebuffers;
	std::unique_ptr<VulkanPipelineCache> pipelines;

	// The frame-slot machinery (WP-17): kFramesInFlight slots, each owning
	// its command buffer, its submit fence, and its acquire (image
	// available) semaphore. A slot's fence is waited at the start of the
	// slot's next use, which orders every resource the slot references
	// (arena regions, descriptor sets, swapchain image) before reuse; the
	// acquire semaphore's only consumer is the slot's own submit, so the
	// same fence wait makes its re-signal safe. All fences are created
	// signaled so the first pass over each slot waits immediately.
	//
	// The release (render finished) semaphores are indexed per swapchain
	// image, not per slot: their consumer is the present, whose completion
	// is only observable through the image itself - vkAcquireNextImageKHR
	// returning an image proves every earlier present of that image
	// finished, so re-signaling the image's semaphore after re-acquiring it
	// is safe by construction (the validation layer's own recommendation;
	// the per-slot version trips VUID-vkQueueSubmit-pSignalSemaphores-00067
	// whenever two presents of one FIFO queue overlap). Sized to the image
	// count in recreate_swapchain.
	VkCommandPool command_pool = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, kFramesInFlight> command_buffers{};
	std::array<VkFence, kFramesInFlight> frame_fences{};
	std::array<VkSemaphore, kFramesInFlight> image_available_semaphores{};
	std::vector<VkSemaphore> render_finished_semaphores;

	// The staging arenas (WP-15), one per frame slot since WP-17: a frame
	// may outlive the next frame's begin_frame, so a single arena reset per
	// frame boundary is no longer safe. current_arena_ names the slot the
	// recording (or between-frames offscreen) work allocates from; it is
	// re-pointed in begin_frame and fed to every created VulkanBuffer by
	// reference. A slot's arena resets at the start of that slot's next use,
	// after its fence wait.
	std::array<std::unique_ptr<VulkanArena>, kFramesInFlight> arenas;
	VulkanArena* current_arena_ = nullptr;

	// WP-16: the texture-upload machinery. One-shot command buffers come from
	// a dedicated pool (never the frame's), and the single fence serializes
	// uploads and makes them finish before a later frame samples the texture.
	VulkanUploadContext upload_context;

	// WP-16: the per-frame descriptor pools, one per frame slot since WP-17
	// (same lifetime argument as the arenas). Sets are allocated at
	// bind_descriptor_set time; a slot's pool resets at the start of that
	// slot's next use, after its fence wait, so no recorded draw references
	// a set from the pool being reset. Each grows itself on exhaustion
	// instead of throwing (D3).
	std::array<std::unique_ptr<VulkanDescriptorPool>, kFramesInFlight> descriptor_pools;

	// WP-16b: the offscreen submit machinery. One-shot command buffers come
	// from a dedicated pool (never the frame's or the upload pool's), and
	// the dedicated fence serializes offscreen submits: submit_offscreen
	// waits on it before returning, so the drawn result is visible to
	// sampling in the current frame and every arena region / descriptor set
	// the submit referenced is retired before the next begin_frame resets
	// them. A separate fence is essential - a nested submit inside a frame
	// must not disturb the frame fence, which is unsignaled at that point.
	VkCommandPool offscreen_command_pool = VK_NULL_HANDLE;
	VkFence offscreen_fence = VK_NULL_HANDLE;

	// The samplers the descriptor writes reference: one per texture filter
	// (every texture in the tree clamps to edge). Created once at device
	// startup; the textures share them by filter.
	VkSampler sampler_linear = VK_NULL_HANDLE;
	VkSampler sampler_nearest = VK_NULL_HANDLE;

	// A 1x1 white texture bound wherever the RHI records a null texture (the
	// blit program's absent mask). Vulkan validation rejects a null image
	// view in a statically used binding, so the write substitutes this.
	std::unique_ptr<VulkanTexture> dummy_texture;

	// Set when begin_frame dropped the frame (swapchain recreation); end_frame
	// then submits nothing.
	bool frame_dropped_ = false;

	// The frame slot the current frame (or the last begun one, between
	// frames) records into; end_frame and begin_offscreen resolve their
	// per-slot resources through it. Advances modulo kFramesInFlight at the
	// end of every presented frame; a dropped frame keeps the slot so it is
	// retried next time.
	uint32_t frame_slot_ = 0;

	// The swapchain image index the current frame acquired and end_frame must
	// present.
	uint32_t frame_image_index_ = 0;

	// The number of *real* frames begin_frame has produced so far (WP-17;
	// D6 fixed this to exclude dropped calls): stamps deferred texture frees
	// so they are released kFramesInFlight real frames after their texture
	// died, when no in-flight frame can reference them anymore. A dropped
	// begin_frame retries the same slot rather than advancing to a fresh
	// one, so it must not advance this counter either - see the comment in
	// begin_frame.
	uint64_t frame_stamp_ = 0;
	std::vector<PendingTextureFree> pending_texture_frees_;

	// The physical device's maxImageDimension2D (WP-16): what the atlas
	// builder sizes itself against under Vulkan.
	uint32_t max_texture_size_ = 0;

	VkSampler create_sampler(TextureFilter filter) const;
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

	// MoltenVK is a portability driver, not a conformant Vulkan
	// implementation: the loader only enumerates it when the instance opts in
	// via VK_KHR_portability_enumeration, otherwise vkCreateInstance below
	// fails with VK_ERROR_INCOMPATIBLE_DRIVER on macOS. Requested only when
	// the loader actually offers the extension (native Vulkan drivers on
	// other platforms do not).
	//
	// VK_KHR_get_physical_device_properties2 is a hard dependency of
	// VK_KHR_portability_subset (enabled further down, once the physical
	// device is known to advertise it): the instance is created at API 1.0,
	// so the promoted-to-1.1 core entry points are not implicitly available
	// and this has to be requested explicitly too.
	bool portability_enumeration_supported = false;
	bool get_physical_device_properties2_supported = false;
	{
		uint32_t extension_count = 0;
		check_vulkan_result(
		   vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr),
		   "vkEnumerateInstanceExtensionProperties");
		std::vector<VkExtensionProperties> extensions(extension_count);
		check_vulkan_result(
		   vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data()),
		   "vkEnumerateInstanceExtensionProperties");
		for (const VkExtensionProperties& extension : extensions) {
			if (strncmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
			            VK_MAX_EXTENSION_NAME_SIZE) == 0) {
				portability_enumeration_supported = true;
			} else if (strncmp(extension.extensionName,
			                   VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
			                   VK_MAX_EXTENSION_NAME_SIZE) == 0) {
				get_physical_device_properties2_supported = true;
			}
		}
	}
	if (portability_enumeration_supported) {
		instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	}
	if (get_physical_device_properties2_supported) {
		instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
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
	if (portability_enumeration_supported) {
		instance_create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}
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
	log_info("Graphics: Vulkan: Frames in flight: %u\n",
	         static_cast<unsigned>(kFramesInFlight));

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
	// VK_KHR_maintenance1 is the second required extension: it allows a
	// negative viewport height, which is how a render-to-texture pass undoes
	// the committed SPIR-V's Y negation (see VulkanCommandBuffer::set_viewport).
	// It is core since Vulkan 1.1 and universally available, but the instance
	// is created at API 1.0, so it has to be enabled explicitly.
	bool portability_subset_supported = false;
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
		bool maintenance1_supported = false;
		for (const VkExtensionProperties& extension : extensions) {
			if (strncmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			            VK_MAX_EXTENSION_NAME_SIZE) == 0) {
				swapchain_supported = true;
			} else if (strncmp(extension.extensionName, VK_KHR_MAINTENANCE1_EXTENSION_NAME,
			                   VK_MAX_EXTENSION_NAME_SIZE) == 0) {
				maintenance1_supported = true;
			} else if (strncmp(extension.extensionName, kPortabilitySubsetExtensionName,
			                   VK_MAX_EXTENSION_NAME_SIZE) == 0) {
				portability_subset_supported = true;
			}
		}
		if (!swapchain_supported) {
			throw wexception("Vulkan: the physical device does not support VK_KHR_swapchain.");
		}
		if (!maintenance1_supported) {
			throw wexception("Vulkan: the physical device does not support VK_KHR_maintenance1.");
		}
	}

	// The spec requires enabling VK_KHR_portability_subset whenever a
	// physical device advertises it (every MoltenVK device does); it is not
	// optional the way the other extensions here are.
	std::vector<const char*> device_extensions = {
	   VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME};
	if (portability_subset_supported) {
		device_extensions.push_back(kPortabilitySubsetExtensionName);
	}
	VkDeviceCreateInfo device_create_info{};
	device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_create_info.queueCreateInfoCount = 1;
	device_create_info.pQueueCreateInfos = &queue_create_info;
	device_create_info.enabledExtensionCount =
	   static_cast<uint32_t>(device_extensions.size());
	device_create_info.ppEnabledExtensionNames = device_extensions.data();
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
	command_buffer_allocate_info.commandBufferCount = kFramesInFlight;
	check_vulkan_result(
	   vkAllocateCommandBuffers(device, &command_buffer_allocate_info, command_buffers.data()),
	   "vkAllocateCommandBuffers");

	// One fence per frame slot, all signaled at creation so the first pass
	// over each slot waits immediately; afterwards a slot's fence is reset
	// per frame and signaled by that frame's submit.
	VkFenceCreateInfo fence_create_info{};
	fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
		check_vulkan_result(vkCreateFence(device, &fence_create_info, nullptr, &frame_fences[slot]),
		                    "vkCreateFence");
	}

	// The acquire semaphore per frame slot (WP-17): the acquire signals it
	// and the slot's own submit waits on it, so the slot fence wait orders
	// its re-signal. The release (render finished) semaphores are per
	// swapchain image and are created with the swapchain in
	// recreate_swapchain, where the image count is known.
	VkSemaphoreCreateInfo semaphore_create_info{};
	semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
		check_vulkan_result(
		   vkCreateSemaphore(device, &semaphore_create_info, nullptr,
		                     &image_available_semaphores[slot]),
		   "vkCreateSemaphore (image available)");
	}

	// The staging arenas (WP-15), one per frame slot (WP-17): allocations are
	// aligned to the device's uniform-buffer-offset alignment so WP-16 can
	// bind any region as a UBO.
	const uint32_t arena_alignment = static_cast<uint32_t>(
	   std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 256u));
	for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
		arenas[slot].reset(new VulkanArena(
		   device, find_arena_memory_type(physical_device), arena_alignment, kInitialArenaSize));
	}
	current_arena_ = arenas[0].get();

	max_texture_size_ = properties.limits.maxImageDimension2D;
	if (max_texture_size_ < kMinimumVulkanTextureSize) {
		throw wexception("Vulkan: maxImageDimension2D (%u) is below the %u the renderer needs",
		                 max_texture_size_, kMinimumVulkanTextureSize);
	}

	// The upload path (WP-16): a dedicated one-shot command pool and a fence
	// that serializes uploads. The physical device travels with it so a
	// sampled texture can pick its own device-local memory type.
	VkCommandPoolCreateInfo upload_pool_create_info{};
	upload_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	upload_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
	                                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	upload_pool_create_info.queueFamilyIndex = queue_family;
	check_vulkan_result(
	   vkCreateCommandPool(device, &upload_pool_create_info, nullptr, &upload_context.command_pool),
	   "vkCreateCommandPool (upload)");
	upload_context.device = device;
	upload_context.physical_device = physical_device;
	upload_context.queue = queue;
	VkFenceCreateInfo upload_fence_create_info{};
	upload_fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	upload_fence_create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	check_vulkan_result(
	   vkCreateFence(device, &upload_fence_create_info, nullptr, &upload_context.fence),
	   "vkCreateFence (upload)");

	// The per-frame descriptor pools (WP-16), one per slot (WP-17). The
	// counts are generous upper bounds for one frame of Widelands: sets are
	// allocated per bind_descriptor_set call (draw batches), a set carries
	// at most 2 samplers + 1 uniform buffer. A slot's pool resets at the
	// start of that slot's next use, and grows itself (a fresh pool of the
	// same size) rather than failing if a scene ever needs more (D3). No
	// individual set is ever freed - only the whole chain is reset together
	// - so VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT would only cost
	// allocator quality and is not set.
	for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
		descriptor_pools[slot].reset(new VulkanDescriptorPool(device, 4096, 4096, 4096));
	}

	// The offscreen submit path (WP-16b): a one-shot command pool and a
	// fence, mirroring the upload path. The fence starts unsignaled - it is
	// reset before every submit anyway - and nothing signals it at startup.
	VkCommandPoolCreateInfo offscreen_pool_create_info{};
	offscreen_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	offscreen_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
	                                   VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	offscreen_pool_create_info.queueFamilyIndex = queue_family;
	check_vulkan_result(vkCreateCommandPool(device, &offscreen_pool_create_info, nullptr,
	                                        &offscreen_command_pool),
	                    "vkCreateCommandPool (offscreen)");
	VkFenceCreateInfo offscreen_fence_create_info{};
	offscreen_fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	check_vulkan_result(vkCreateFence(device, &offscreen_fence_create_info, nullptr,
	                                  &offscreen_fence),
	                    "vkCreateFence (offscreen)");

	sampler_linear = create_sampler(TextureFilter::kLinear);
	sampler_nearest = create_sampler(TextureFilter::kNearest);

	// The 1x1 white dummy texture for null texture bindings (the blit
	// program's absent mask), uploaded once at startup through the real
	// upload path.
	const uint8_t kWhitePixel[4] = {255, 255, 255, 255};
	dummy_texture.reset(new VulkanTexture(
	   device, 1, 1, TextureFormat::kRGBA8, TextureFilter::kLinear, sampler_linear, &upload_context));
	dummy_texture->upload(kWhitePixel);

	if (!recreate_swapchain()) {
		throw wexception("Vulkan: the window has no drawable size.");
	}
}

VkSampler VulkanDevice::Impl::create_sampler(const TextureFilter filter) const {
	// Every texture in the tree clamps to edge (texture.cc sets
	// GL_CLAMP_TO_EDGE for all of them), so the wrap mode is a constant; only
	// the filter varies, and the RHI declares exactly two of those.
	VkSamplerCreateInfo sampler_create_info{};
	sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_create_info.magFilter =
	   filter == TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	sampler_create_info.minFilter = sampler_create_info.magFilter;
	sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_create_info.maxLod = 0.0f;
	VkSampler sampler = VK_NULL_HANDLE;
	check_vulkan_result(vkCreateSampler(device, &sampler_create_info, nullptr, &sampler),
	                    "vkCreateSampler");
	return sampler;
}

VulkanDevice::Impl::~Impl() {
	vkDeviceWaitIdle(device);
	// The arenas must go after the idle wait (recorded draws reference their
	// regions) and before the device.
	for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
		arenas[slot].reset();
		vkDestroyFence(device, frame_fences[slot], nullptr);
		vkDestroySemaphore(device, image_available_semaphores[slot], nullptr);
		descriptor_pools[slot].reset();
	}
	for (VkSemaphore semaphore : render_finished_semaphores) {
		vkDestroySemaphore(device, semaphore, nullptr);
	}
	render_finished_semaphores.clear();
	vkDestroyCommandPool(device, command_pool, nullptr);
	for (VkFramebuffer framebuffer : framebuffers) {
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	}
	framebuffers.clear();
	for (VkImageView color_view : color_views) {
		vkDestroyImageView(device, color_view, nullptr);
	}
	color_views.clear();
	for (VkImageView depth_view : depth_views) {
		vkDestroyImageView(device, depth_view, nullptr);
	}
	depth_views.clear();
	for (VkImage depth_image : depth_images) {
		vkDestroyImage(device, depth_image, nullptr);
	}
	depth_images.clear();
	for (VkDeviceMemory depth_memory : depth_memories) {
		vkFreeMemory(device, depth_memory, nullptr);
	}
	depth_memories.clear();
	// The pipeline cache owns render-pass-bound resources and must die before
	// the swapchain-independent Vulkan objects (and the device).
	pipelines.reset();
	vkDestroySwapchainKHR(device, swapchain, nullptr);
	if (debug_messenger != VK_NULL_HANDLE) {
		vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, nullptr);
	}
	vkDestroySurfaceKHR(instance, surface, nullptr);
	// WP-16 objects: the dummy texture (whose upload command buffers are long
	// submitted and waited), the samplers, the upload staging buffer, fence
	// and pool. All die after the idle wait and before the device. The
	// WP-16b offscreen pool and fence join them.
	dummy_texture.reset();
	// Every deferred texture free is safe now (device idle); the dummy
	// texture above just queued its own. Must run before vkDestroyDevice.
	drain_pending_texture_frees(true);
	vkDestroySampler(device, sampler_linear, nullptr);
	vkDestroySampler(device, sampler_nearest, nullptr);
	if (upload_context.staging_buffer != VK_NULL_HANDLE) {
		vkUnmapMemory(device, upload_context.staging_memory);
		vkDestroyBuffer(device, upload_context.staging_buffer, nullptr);
		vkFreeMemory(device, upload_context.staging_memory, nullptr);
	}
	vkDestroyFence(device, upload_context.fence, nullptr);
	vkDestroyCommandPool(device, upload_context.command_pool, nullptr);
	vkDestroyFence(device, offscreen_fence, nullptr);
	vkDestroyCommandPool(device, offscreen_command_pool, nullptr);
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

	// FIFO (vsync) is mandatory in every implementation. The game caps its
	// refresh at 30 FPS (ui/basic/panel.cc), so a faster present mode would
	// only burn power; WP-17 keeps FIFO deliberately.
	const VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

	// The frame renders through the screen render pass (WP-14), so the images
	// need only the mandatory COLOR_ATTACHMENT usage.
	constexpr VkImageUsageFlags kSwapchainUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((capabilities.supportedUsageFlags & kSwapchainUsage) != kSwapchainUsage) {
		throw wexception("Vulkan: the surface does not support COLOR_ATTACHMENT image usage.");
	}

	// The old swapchain must not be destroyed while any queue is still
	// executing work on its images - including queued presents. Since WP-17
	// the frame loop runs frames in flight, so a queue-level wait (not a
	// device-wide stall) drains every pending submit and present on this
	// queue; the per-slot fences end up signaled, so the next begin_frame's
	// fence wait passes immediately.
	check_vulkan_result(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

	// The release semaphores are rebuilt with the new image count below;
	// the queue wait above guarantees no submit signal or pending present
	// references the old ones anymore.
	for (VkSemaphore semaphore : render_finished_semaphores) {
		vkDestroySemaphore(device, semaphore, nullptr);
	}
	render_finished_semaphores.clear();

	// The framebuffers reference the swapchain image views, the depth views
	// and the render pass, so they must go before the old swapchain - and
	// before the render pass, when the format change below rebuilds it. The
	// depth attachments are recreated too (they are sized to the swapchain
	// extent).
	for (VkFramebuffer framebuffer : framebuffers) {
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	}
	framebuffers.clear();
	for (VkImageView color_view : color_views) {
		vkDestroyImageView(device, color_view, nullptr);
	}
	color_views.clear();
	for (VkImageView depth_view : depth_views) {
		vkDestroyImageView(device, depth_view, nullptr);
	}
	depth_views.clear();
	for (VkImage depth_image : depth_images) {
		vkDestroyImage(device, depth_image, nullptr);
	}
	depth_images.clear();
	for (VkDeviceMemory depth_memory : depth_memories) {
		vkFreeMemory(device, depth_memory, nullptr);
	}
	depth_memories.clear();

	// The screen pipelines bake in the screen render pass, and that render
	// pass bakes in the swapchain image format, so a format change is the one
	// reason to rebuild them; a plain resize keeps them (viewports are
	// dynamic state). The rebuild only touches the screen render pass and
	// its pipelines - descriptor/pipeline layouts and the offscreen render
	// pass are format-independent and are left alone, so live textures and
	// descriptor sets (which resolved handles from this cache before the
	// rebuild) stay valid across it (Phase D review, finding D2).
	const bool format_changed = image_format != surface_format.format;
	image_format = surface_format.format;
	if (pipelines == nullptr) {
		pipelines.reset(new VulkanPipelineCache(device, image_format, depth_format));
	} else if (format_changed) {
		pipelines->rebuild_screen_pipelines(image_format, depth_format);
	}

	// Request kFramesInFlight + 1 images (WP-17): with two frames in flight
	// and FIFO, two images can be held (one rendering, one presenting) and a
	// third keeps vkAcquireNextImageKHR from blocking. Some drivers grant
	// fewer than requested - the acquire then stalls, but stays correct.
	const uint32_t requested_image_count = std::max(
	   capabilities.minImageCount, kFramesInFlight + 1);
	const uint32_t min_image_count = capabilities.maxImageCount == 0 ?
	                                    requested_image_count :
	                                    std::min(requested_image_count, capabilities.maxImageCount);

	VkSwapchainCreateInfoKHR swapchain_create_info{};
	swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchain_create_info.surface = surface;
	swapchain_create_info.minImageCount = min_image_count;
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
	// A native window can only be associated with one non-retired swapchain,
	// and oldSwapchain is the retirement mechanism: without it the creation
	// below fails with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR on every recreation
	// (NVIDIA enforces this; Mesa does not). VK_NULL_HANDLE on the first call.
	swapchain_create_info.oldSwapchain = swapchain;

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

	// The release semaphores are indexed by swapchain image (see the member
	// comment): re-signaling one is safe because the image can only be
	// re-acquired after its previous present completed. Rebuilt here since
	// the image count can change; the old ones were destroyed after the
	// queue wait at the top (nothing references them then).
	VkSemaphoreCreateInfo semaphore_create_info{};
	semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (uint32_t i = 0; i < image_count; ++i) {
		VkSemaphore semaphore = VK_NULL_HANDLE;
		check_vulkan_result(
		   vkCreateSemaphore(device, &semaphore_create_info, nullptr, &semaphore),
		   "vkCreateSemaphore (render finished)");
		render_finished_semaphores.push_back(semaphore);
	}

	create_depth_and_framebuffers(pipelines->render_pass());

	verb_log_info("Graphics: Vulkan: Swapchain: %dx%d (format %u, %u images)\n", extent.width,
	              extent.height, static_cast<unsigned>(image_format), image_count);
	return true;
}

void VulkanDevice::Impl::create_depth_and_framebuffers(const VkRenderPass render_pass) {
	// One framebuffer per swapchain image, and since WP-17 one depth
	// attachment per swapchain image too: the screen pass clears depth, so
	// two overlapping frames rendering into a shared depth image would race.
	// Per-image depth keeps each frame's attachment usage isolated.
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

	for (const VkImage image : swapchain_images) {
		// The depth attachment for this image.
		VkImage depth_image = VK_NULL_HANDLE;
		check_vulkan_result(vkCreateImage(device, &image_create_info, nullptr, &depth_image),
		                    "vkCreateImage");
		depth_images.push_back(depth_image);

		VkMemoryRequirements memory_requirements{};
		vkGetImageMemoryRequirements(device, depth_image, &memory_requirements);
		VkMemoryAllocateInfo allocate_info{};
		allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate_info.allocationSize = memory_requirements.size;
		allocate_info.memoryTypeIndex =
		   find_memory_type(physical_device, memory_requirements.memoryTypeBits,
		                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory depth_memory = VK_NULL_HANDLE;
		check_vulkan_result(vkAllocateMemory(device, &allocate_info, nullptr, &depth_memory),
		                    "vkAllocateMemory");
		depth_memories.push_back(depth_memory);
		check_vulkan_result(vkBindImageMemory(device, depth_image, depth_memory, 0),
		                    "vkBindImageMemory");

		VkImageViewCreateInfo depth_view_create_info{};
		depth_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		depth_view_create_info.image = depth_image;
		depth_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		depth_view_create_info.format = depth_format;
		depth_view_create_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		VkImageView depth_view = VK_NULL_HANDLE;
		check_vulkan_result(
		   vkCreateImageView(device, &depth_view_create_info, nullptr, &depth_view),
		   "vkCreateImageView");
		depth_views.push_back(depth_view);

		// The framebuffer: this image's colour view plus its depth view. Both
		// views are kept (framebuffers do not retain image views) and
		// destroyed with the framebuffers on the next recreation.
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

		const VkImageView attachments[2] = {color_view, depth_view};
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

void VulkanDevice::Impl::retire_texture_resources(const VkImage image,
                                                   const VkDeviceMemory memory,
                                                   const VkImageView view) {
	// Stamped with the current frame number; freed by
	// drain_pending_texture_frees once kFramesInFlight more frames have
	// begun (a frame recorded before the stamp is long completed then).
	pending_texture_frees_.push_back(PendingTextureFree{image, memory, view, frame_stamp_});
}

void VulkanDevice::Impl::drain_pending_texture_frees(const bool free_all) {
	// Called after begin_frame's fence wait (so the work of frame
	// frame_stamp_ - kFramesInFlight has completed) or, with free_all, from
	// the destructor after the device idle wait.
	auto entry = pending_texture_frees_.begin();
	while (entry != pending_texture_frees_.end()) {
		if (!free_all && entry->frame_stamp + kFramesInFlight > frame_stamp_) {
			break;
		}
		vkDestroyImageView(device, entry->view, nullptr);
		vkFreeMemory(device, entry->memory, nullptr);
		vkDestroyImage(device, entry->image, nullptr);
		entry = pending_texture_frees_.erase(entry);
	}
}

std::unique_ptr<CommandBuffer> VulkanDevice::Impl::begin_frame() {
	// Frames in flight (WP-17): kFramesInFlight slots rotate. Waiting on
	// this slot's fence guarantees the queue is done with everything the
	// slot's previous frame recorded - its arena regions, its descriptor
	// sets and its swapchain image - before any of them are reused. The
	// acquire's semaphore (not a fence, and no blocking wait) orders the
	// presentation engine's read of the image against the submit that
	// renders into it.
	const uint32_t slot = frame_slot_;
	check_vulkan_result(vkWaitForFences(device, 1, &frame_fences[slot], VK_TRUE, UINT64_MAX),
	                    "vkWaitForFences");
	// The fence wait completed the frame from kFramesInFlight ago - the last
	// one that could reference a deferred texture free stamped before this
	// frame began. frame_stamp_ is not incremented yet (D6): it only counts
	// frames that actually reach a fresh fence-wait on their own slot, and a
	// dropped frame below retries the same slot rather than advancing to
	// one, so counting it here would let the age check in
	// drain_pending_texture_frees free an entry before kFramesInFlight
	// distinct slots have genuinely been waited on since it was retired.
	drain_pending_texture_frees(false);
	arenas[slot]->reset();
	current_arena_ = arenas[slot].get();
	// The slot's descriptor pool has the same lifetime as the slot's arena
	// (WP-16): every set a recorded draw references came from this pool (or
	// a pool it grew into, D3), and the fence wait above guarantees the
	// queue is done with all of them.
	descriptor_pools[slot]->reset();

	uint32_t image_index = 0;
	const VkResult acquire_result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
	                                                      image_available_semaphores[slot],
	                                                      VK_NULL_HANDLE, &image_index);
	if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR || acquire_result == VK_SUBOPTIMAL_KHR) {
		// Window was resized; the swapchain no longer matches the surface.
		// Recreate and drop this frame. The slot fence stays signaled (it is
		// reset only after a successful acquire), so the next attempt's wait
		// passes immediately, and the slot is not advanced - it is retried.
		// The frame is dropped either way, so whether the surface turned out
		// to still be unusable (minimized) does not change what happens here.
		static_cast<void>(recreate_swapchain());
		frame_dropped_ = true;
		return std::unique_ptr<CommandBuffer>(new VulkanNoOpCommandBuffer());
	}
	if (acquire_result != VK_SUCCESS) {
		log_warn("Vulkan: skipping a frame: vkAcquireNextImageKHR: %s\n",
		         vulkan_result_string(acquire_result));
		frame_dropped_ = true;
		return std::unique_ptr<CommandBuffer>(new VulkanNoOpCommandBuffer());
	}
	// The fence is reset only now: it must stay signaled on a dropped frame
	// (above), or the next wait on this slot would block forever.
	check_vulkan_result(vkResetFences(device, 1, &frame_fences[slot]), "vkResetFences");

	check_vulkan_result(
	   vkResetCommandBuffer(command_buffers[slot], 0), "vkResetCommandBuffer");
	VkCommandBufferBeginInfo command_buffer_begin_info{};
	command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vulkan_result(
	   vkBeginCommandBuffer(command_buffers[slot], &command_buffer_begin_info),
	   "vkBeginCommandBuffer");

	frame_dropped_ = false;
	// Only a real frame - one that reaches its own fresh fence-wait above -
	// advances the retirement clock (D6).
	++frame_stamp_;
	frame_image_index_ = image_index;
	const VulkanCommandBuffer::Target target{
	   pipelines->render_pass(), framebuffers[image_index], extent, 2};
	return std::unique_ptr<CommandBuffer>(new VulkanCommandBuffer(
	   device, command_buffers[slot], pipelines.get(), target, descriptor_pools[slot].get(),
	   dummy_texture.get()));
}

void VulkanDevice::Impl::end_frame(std::unique_ptr<CommandBuffer> command_buffer_wrapper) {
	if (frame_dropped_) {
		frame_dropped_ = false;
		return;
	}
	const uint32_t slot = frame_slot_;
	auto* recorded = static_cast<VulkanCommandBuffer*>(command_buffer_wrapper.get());
	recorded->finish();

	// The submit waits on the acquire's semaphore (the presentation engine is
	// done with the image), signals the image's release semaphore for the
	// present and the slot fence for the slot's next use. No CPU wait here -
	// that is the point of frames in flight; the fence is only waited when
	// the slot comes around again in begin_frame, and the release semaphore
	// is re-signaled only after its image is re-acquired (which guarantees
	// the previous present of that image completed).
	const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = &image_available_semaphores[slot];
	submit_info.pWaitDstStageMask = &wait_stage;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffers[slot];
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &render_finished_semaphores[frame_image_index_];
	check_vulkan_result(
	   vkQueueSubmit(queue, 1, &submit_info, frame_fences[slot]), "vkQueueSubmit");

	VkPresentInfoKHR present_info{};
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = &render_finished_semaphores[frame_image_index_];
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &swapchain;
	present_info.pImageIndices = &frame_image_index_;
	const VkResult present_result = vkQueuePresentKHR(queue, &present_info);
	// The slot was consumed either way - a dropped begin_frame is the only
	// path that leaves it in place for a retry.
	frame_slot_ = (slot + 1) % kFramesInFlight;
	if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
		// Nothing left to do with this frame regardless of whether the
		// surface turned out to still be unusable (minimized): the next
		// begin_frame's acquire is what decides whether to keep dropping.
		static_cast<void>(recreate_swapchain());
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

uint32_t VulkanDevice::max_texture_size() const {
	return impl_->max_texture_size_;
}

void VulkanDevice::notify_resolution_changed() {
	// recreate_swapchain() re-queries the real surface extent itself
	// (vkGetPhysicalDeviceSurfaceCapabilitiesKHR / SDL_Vulkan_GetDrawableSize),
	// so nothing here needs the new size - just the prompt to go look.
	// False means the window is currently minimized, which is not an error:
	// the frame loop already skips frames on a zero-size surface.
	static_cast<void>(impl_->recreate_swapchain());
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
	// WP-16b: the real immediate render-to-texture path. A one-shot command
	// buffer from the dedicated offscreen pool (the frame's buffer may be
	// mid-recording - font and image-cache draws are nested inside frames -
	// so it is never borrowed), begun immediately, recording into whatever
	// texture the caller transitions and begins a pass on. The screen target
	// stays empty: an offscreen buffer never targets the swapchain.
	VkCommandBufferAllocateInfo allocate_info{};
	allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate_info.commandPool = impl_->offscreen_command_pool;
	allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate_info.commandBufferCount = 1;
	VkCommandBuffer commands = VK_NULL_HANDLE;
	check_vulkan_result(vkAllocateCommandBuffers(impl_->device, &allocate_info, &commands),
	                    "vkAllocateCommandBuffers (offscreen)");
	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	check_vulkan_result(vkBeginCommandBuffer(commands, &begin_info),
	                    "vkBeginCommandBuffer (offscreen)");
	std::unique_ptr<CommandBuffer> command_buffer(new VulkanCommandBuffer(
	   impl_->device, commands, impl_->pipelines.get(), VulkanCommandBuffer::Target{},
	   impl_->descriptor_pools[impl_->frame_slot_].get(), impl_->dummy_texture.get()));
	push_command_buffer(command_buffer.get());
	return command_buffer;
}

void VulkanDevice::submit_offscreen(std::unique_ptr<CommandBuffer> command_buffer) {
	// Submit and fence-wait before returning: the contract (rhi.h) requires
	// the recorded result to be visible to sampling in the current frame,
	// and the wait also guarantees every arena region and descriptor set the
	// buffer referenced is retired before the next begin_frame resets them.
	// The submit never touches the frame fence - a nested offscreen submit
	// inside a frame would otherwise corrupt the frame's own wait. Queue
	// order then makes the offscreen writes visible to the frame's later
	// draw calls, which sample the texture the pass just rendered.
	auto* recorded = static_cast<VulkanCommandBuffer*>(command_buffer.get());
	recorded->finish();
	const VkCommandBuffer commands = recorded->vk_command_buffer();
	check_vulkan_result(vkResetFences(impl_->device, 1, &impl_->offscreen_fence),
	                    "vkResetFences (offscreen)");
	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &commands;
	check_vulkan_result(vkQueueSubmit(impl_->queue, 1, &submit_info, impl_->offscreen_fence),
	                    "vkQueueSubmit (offscreen)");
	check_vulkan_result(vkWaitForFences(impl_->device, 1, &impl_->offscreen_fence, VK_TRUE,
	                                    UINT64_MAX),
	                    "vkWaitForFences (offscreen)");
	vkFreeCommandBuffers(impl_->device, impl_->offscreen_command_pool, 1, &commands);
	// Popping the stack restores whatever command buffer was recording
	// before this offscreen submit (a frame's, when nested inside one).
	pop_command_buffer();
}

void VulkanDevice::read_back_swapchain(uint8_t* /* pixels */) {
	throw wexception("Rhi::VulkanDevice::read_back_swapchain: swapchain readback is WP-18");
}

std::unique_ptr<Texture> VulkanDevice::create_texture(const TextureDescriptor& desc) {
	if (desc.width == 0 || desc.height == 0) {
		throw wexception("Rhi::VulkanDevice::create_texture: zero-size texture");
	}
	if (desc.width > impl_->max_texture_size_ || desc.height > impl_->max_texture_size_) {
		throw wexception("Rhi::VulkanDevice::create_texture: %ux%u exceeds maxImageDimension2D %u",
		                 desc.width, desc.height, impl_->max_texture_size_);
	}
	const VkSampler sampler = desc.filter == TextureFilter::kLinear ?
	                             impl_->sampler_linear :
	                             impl_->sampler_nearest;
	// Every RGBA8 texture is a potential immediate render-to-texture target
	// (WP-16b), so it carries the device's shared offscreen render pass;
	// the per-texture framebuffer is built lazily on first use. The retire
	// callback routes the image resources into the deferred-free queue
	// (WP-17): destroying them immediately would race in-flight frames that
	// still reference the texture's view through recorded descriptor sets.
	return std::unique_ptr<Texture>(new VulkanTexture(
	   impl_->device, desc.width, desc.height, desc.format, desc.filter, sampler,
	   &impl_->upload_context, impl_->pipelines->offscreen_render_pass(),
	   [this](const VkImage image, const VkDeviceMemory memory, const VkImageView view) {
		   impl_->retire_texture_resources(image, memory, view);
	   }));
}

std::unique_ptr<Texture>
VulkanDevice::create_texture_view(Texture& /* parent */, const Recti& /* subrect */) {
	throw wexception("Rhi::VulkanDevice::create_texture_view: sub-textures are modelled by BlitData");
}

std::unique_ptr<Buffer> VulkanDevice::create_buffer(const uint32_t /* size */,
                                                    const BufferUsage /* usage */) {
	// Both vertex and uniform data live in the staging arenas; the capacity
	// hint is ignored (see VulkanBuffer). The buffer routes through the
	// device's current-arena pointer, so update() always lands in the arena
	// of the frame slot that is currently recording (WP-17).
	return std::unique_ptr<Buffer>(new VulkanBuffer(impl_->current_arena_));
}

std::unique_ptr<Pipeline> VulkanDevice::create_pipeline(const PipelineDescriptor& desc) {
	const bool requires_binding = impl_->pipelines->has_descriptor_bindings(desc.program_name);
	return std::unique_ptr<Pipeline>(
	   new VulkanPipeline(desc.program_name, desc.blend, requires_binding));
}

std::unique_ptr<DescriptorSet> VulkanDevice::create_descriptor_set(const Pipeline& pipeline) {
	const VulkanPipeline& vulkan_pipeline = static_cast<const VulkanPipeline&>(pipeline);
	const std::string& program_name = vulkan_pipeline.program_name();
	// The set carries the pieces the command buffer's bind path needs: the
	// manifest entry (binding translation), the set layout (allocation) and
	// the pipeline layout (binding). All come from the pipeline cache, which
	// built them from the manifest at startup.
	const ManifestProgram* manifest = impl_->pipelines->manifest_program(program_name);
	if (manifest == nullptr) {
		throw wexception("Vulkan: program '%s' has no bindings manifest entry",
		                 program_name.c_str());
	}
	return std::unique_ptr<DescriptorSet>(new VulkanDescriptorSet(
	   program_name, manifest, impl_->pipelines->descriptor_set_layout(program_name),
	   impl_->pipelines->pipeline_layout(program_name), vulkan_pipeline.requires_binding()));
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

uint32_t VulkanDevice::max_texture_size() const {
	return 0;
}

void VulkanDevice::notify_resolution_changed() {
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
