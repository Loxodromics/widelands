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

#ifndef WL_GRAPHIC_RHI_VULKAN_VULKAN_DEVICE_H
#define WL_GRAPHIC_RHI_VULKAN_VULKAN_DEVICE_H

#include <memory>

#include "base/macros.h"

struct SDL_Window;

// The Vulkan bootstrap device (renderer modernization plan, WP-12, WP-14): an
// instance, a physical/logical device, a surface and a swapchain wired to the
// SDL window. Since WP-14 it also owns the screen render pass (colour +
// depth), the depth attachment and framebuffers, and the pipeline cache -
// the twelve VkPipeline objects pre-built from the pipeline catalog - and
// presents the placeholder clear through a real render pass. volk
// (src/third_party/volk) is the loader.
//
// Deliberately *not* a Rhi::Device yet, and not registered with
// Rhi::set_device: the eight programs would route their draws into an
// implementation that has no command recording. WP-15 converts this class
// into the Vulkan Rhi::Device; until then the GL pipeline keeps rendering -
// invisibly, into the GL backbuffer of the hidden window - as a stand-in so
// all game machinery keeps working, and only the presentation is swapped
// over to Vulkan by Graphic::refresh.
//
// All Vulkan types stay out of this header (pimpl); only the .cc includes
// volk and the Vulkan headers.
namespace Rhi {

class VulkanDevice {
public:
	// Creates the instance, device, surface and swapchain for 'window' (which
	// must have been created with the SDL_WINDOW_VULKAN flag). Throws
	// wexception on any failure, so the game fails with a clear error rather
	// than half-initializing.
	explicit VulkanDevice(SDL_Window* window);
	~VulkanDevice();

	// Acquires the next swapchain image, clears it to the placeholder colour
	// and presents it. Called once per Graphic::refresh from the UI thread.
	void present();

	DISALLOW_COPY_AND_ASSIGN(VulkanDevice);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_DEVICE_H
