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

#ifndef WL_GRAPHIC_GL_SYSTEM_HEADERS_H
#define WL_GRAPHIC_GL_SYSTEM_HEADERS_H

// This includes the correct OpenGL headers for us. Use this
// instead of including any system OpenGL headers yourself.
//
// The loader is glad2 (src/third_party/glad2), vendored and generated for GL 3.3
// compatibility profile + GLES 3.0 in one merged config (renderer modernization
// WP-1, Claude/RENDERER_MODERNIZATION_PLAN.md). Compatibility profile, not core,
// because the legacy GL 2.1 path still uses GL_INTENSITY (texture.cc), a token
// core-profile headers omit; see src/third_party/README for the full rationale.
#include <glad/gl.h>

#endif  // end of include guard: WL_GRAPHIC_GL_SYSTEM_HEADERS_H
