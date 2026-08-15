/*
 * Copyright (C) 2006-2026 by the Widelands Development Team
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

#ifndef WL_GRAPHIC_GL_INITIALIZE_H
#define WL_GRAPHIC_GL_INITIALIZE_H

#include <SDL_video.h>

#include "graphic/gl/system_headers.h"

namespace Gl {

// The OpenGL context flavour the game is running with. WP-4 of the renderer
// modernization plan (Claude/RENDERER_MODERNIZATION_PLAN.md) requests a real
// GL 3.3 core context for kOpenGLCore and falls back to kOpenGL21 if the
// driver cannot provide one. This is the GL-internal value: the user-facing
// --renderer selection is RenderBackend (graphic/render_backend.h), which is
// mapped onto this by Graphic::initialize.
enum class Backend {
	kOpenGL21,
	kOpenGLCore,
};

/// The backend that was actually created, i.e. after any fallback. Recorded by
/// initialize(); the rest of the GL code branches on it.
Backend backend();

// Initializes OpenGL. Creates a context for 'window' using SDL and loads the
// GL library. Fills in 'max_texture_size' and returns the created SDL_Context
// which must be closed by the caller.
// 'requested_backend' is the GL flavour Graphic::initialize mapped the
// --renderer selection to; what was actually created is available via
// backend() afterwards.
// If 'trace' is kYes, sets up tracing for OpenGL and outputs every single
// OpenGL call made, together with the result from glGetError (via glad2's
// per-call debug hook). Argument and return values are deliberately not
// decoded: glad2's debug callback carries no per-argument type information.
enum class Trace {
	kYes,
	kNo,
};
SDL_GLContext initialize(const Trace& trace,
                         SDL_Window* window,
                         GLint* max_texture_size,
                         Backend requested_backend);

}  // namespace Gl

#endif  // end of include guard: WL_GRAPHIC_GL_INITIALIZE_H
