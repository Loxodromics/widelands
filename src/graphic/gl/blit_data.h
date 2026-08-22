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

#ifndef WL_GRAPHIC_GL_BLIT_DATA_H
#define WL_GRAPHIC_GL_BLIT_DATA_H

#include <cstdint>

#include "base/rect.h"
#include "graphic/rhi/rhi.h"

// Information needed to properly blit this image. 'texture' is the opaque
// RHI handle the draw path binds; 'texture_id' is the real GL object name
// backing it, kept in sync alongside 'texture' for the calls
// (glGenTextures/glDeleteTextures, Gl::State's bind cache) that still need
// the raw GL name.
struct BlitData {
	// The parent texture, as the RHI's opaque handle. This is either
	// - the packed texture of the texture atlas, if this texture is part of a texture atlas.
	// - the texture itself if it is a standalone texture.
	const Rhi::Texture* texture = nullptr;

	// The OpenGL name of the parent texture that 'texture' wraps.
	uint32_t texture_id = 0;

	// Dimension of the parent texture, For stand alone textures this is the
	// dimensions of the texture itself and therefore equal to
	// rect.[w|h].
	int parent_width = 0;
	int parent_height = 0;

	// The subrect in the parent texture.
	Rectf rect = Rectf();
};

// Texture identity for batching and the render-queue sort key: the RHI
// texture's dense id, or 0 if this BlitData names no texture (e.g. an unset
// mask, see has_texture() below). Callers key on batch_id() rather than
// reaching into 'texture' directly (C3).
inline uint32_t batch_id(const BlitData& data) {
	return data.texture != nullptr ? data.texture->id() : 0;
}

// True if this BlitData names a real texture.
inline bool has_texture(const BlitData& data) {
	return data.texture != nullptr;
}

#endif  // end of include guard: WL_GRAPHIC_GL_BLIT_DATA_H
