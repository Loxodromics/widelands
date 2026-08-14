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

#include <optional>
#include <string>

#include <SDL_video.h>

#include "graphic/gl/system_headers.h"

namespace Gl {

// The rendering backend the game was started with. WP-3 of the renderer
// modernization plan (Claude/RENDERER_MODERNIZATION_PLAN.md) selects it via
// --renderer; WP-4 requests a real GL 3.3 core context for kOpenGLCore and
// falls back to kOpenGL21 if the driver cannot provide one.
enum class Backend {
	kOpenGL21,
	kOpenGLCore,
};

/// Maps the --renderer command line value ("gl21", "glcore") to a Backend.
/// Returns std::nullopt for unknown names.
std::optional<Backend> backend_from_string(const std::string& name);
/// The --renderer name of a backend, for logging and the dev harness.
const char* backend_name(Backend backend);

/// The backend that was actually created, i.e. after any fallback. Recorded by
/// initialize(); the harness reads it from the log line, the rest of the code
/// branches on it.
Backend backend();

// Initializes OpenGL. Creates a context for 'window' using SDL and loads the
// GL library. Fills in 'max_texture_size' and returns the created SDL_Context
// which must be closed by the caller.
// 'requested_backend' is what --renderer asked for; what was actually created
// is available via backend() afterwards.
// If 'trace' is kYes, sets up tracing for OpenGL and outputs every single
// OpenGL call made, together with its arguments, return value and the result
// from glGetError (via glad2's per-call debug hook).
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
