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

#ifndef WL_GRAPHIC_RHI_DEVICE_H
#define WL_GRAPHIC_RHI_DEVICE_H

#include "graphic/rhi/rhi.h"

// The backend-neutral device registry. This is what backend-neutral code
// (the eight programs, the render queue, the texture draw path) includes to
// reach the active RHI device; it deliberately does not include GL or Vulkan
// headers, unlike the per-backend rhi/gl/... headers.
//
// The active device is registered by its backend's constructor (GlCoreDevice
// today, the Vulkan device in WP-12) via set_device() and unregistered by its
// destructor, so a backend registers itself rather than writing into a shared
// file-static owned by some other backend's translation unit.
namespace Rhi {

// True once a backend has registered its Device (i.e. the core path, today;
// Vulkan later). The backend-neutral replacement for branching on
// Gl::backend(): "is there an RHI device active" is the question the draw code
// actually wants answered, not "is this the GL-core backend".
bool has_device();

// The active device. Throws wexception if none is registered.
Device& device();

// The command buffer currently being recorded (the top of the device's
// frame/offscreen stack). Throws wexception if no device is registered or no
// command buffer is being recorded.
CommandBuffer& command_buffer();

// Registers / unregisters the active device. Called by the backend's
// constructor (with itself) and destructor (with nullptr).
void set_device(Device* device);

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_DEVICE_H
