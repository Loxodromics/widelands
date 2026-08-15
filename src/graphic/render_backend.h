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

#ifndef WL_GRAPHIC_RENDER_BACKEND_H
#define WL_GRAPHIC_RENDER_BACKEND_H

#include <optional>
#include <string>

// The rendering backend the game runs on, selected by --renderer (WP-3 of the
// renderer modernization plan, Claude/RENDERER_MODERNIZATION_PLAN.md). This is
// the backend-neutral selector across all backends; Gl::Backend
// (graphic/gl/initialize.h) is the GL-internal context flavour and is derived
// from this. WP-19 gains the automatic fallback chain (Vulkan -> GL-core ->
// GL 2.1) on top of this enum, and WP-12 adds the first Vulkan bootstrap.
enum class RenderBackend {
	kVulkan,
	kOpenGLCore,
	kOpenGL21,
};

/// Maps the --renderer command line value ("vulkan", "glcore", "gl21") to a
/// RenderBackend. Returns std::nullopt for unknown names.
std::optional<RenderBackend> render_backend_from_string(const std::string& name);

/// Whether this build can create the backend at all. The GL backends are
/// always available; Vulkan requires OPTION_BUILD_VULKAN.
bool render_backend_available(RenderBackend backend);

/// The --renderer name of a backend, for logging and the dev harness.
const char* render_backend_name(RenderBackend backend);

/// The backend that was actually created, i.e. after any fallback. Recorded by
/// Graphic::initialize(); the harness reads it from the log line
/// "Graphics: Render backend: <name>", the rest of the code branches on it via
/// Rhi::has_device() / Gl::backend().
RenderBackend obtained_render_backend();

/// Records the obtained backend (see above). Called by Graphic::initialize.
void record_obtained_render_backend(RenderBackend backend);

#endif  // end of include guard: WL_GRAPHIC_RENDER_BACKEND_H
