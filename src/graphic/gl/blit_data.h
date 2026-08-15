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

// Information needed to properly blit this image. It is backend-neutral in
// shape: 'texture' is the opaque parent texture on the RHI core path, and
// 'texture_id' is the raw GL name the frozen legacy 2.1 path still consumes.
// The two are set together on the core path (texture_id mirrors the GL name of
// 'texture'), while only texture_id is set on the legacy path where no RHI
// device exists.
struct BlitData {
	// The parent texture, as the RHI's opaque handle. This is either
	// - the packed texture of the texture atlas, if this texture is part of a texture atlas.
	// - the texture itself if it is a standalone texture.
	// Null on the legacy 2.1 path.
	const Rhi::Texture* texture = nullptr;

	// The OpenGL name or id of the parent texture. The legacy 2.1 path reads
	// this; the RHI core path keeps it in sync with 'texture' but draws through
	// 'texture'.
	uint32_t texture_id = 0;

	// Dimension of the parent texture, For stand alone textures this is the
	// dimensions of the texture itself and therefore equal to
	// rect.[w|h].
	int parent_width = 0;
	int parent_height = 0;

	// The subrect in the parent texture.
	Rectf rect = Rectf();
};

// Backend-neutral texture identity for batching and the render-queue sort key:
// the RHI texture's dense id on the core path, the raw GL name on the frozen
// legacy path (where no RHI device exists and only texture_id is populated).
// These two helpers are the only place the "which field is authoritative"
// branch lives; callers key on batch_id() rather than reaching into either
// field (C3).
inline uint32_t batch_id(const BlitData& data) {
	return data.texture != nullptr ? data.texture->id() : data.texture_id;
}

// True if this BlitData names a real texture (the backend-neutral form of
// texture_id == 0). On the core path both fields are set together; on the
// legacy path only texture_id is set and 'texture' stays null, so both fields
// must be consulted.
inline bool has_texture(const BlitData& data) {
	return data.texture != nullptr || data.texture_id != 0;
}

#endif  // end of include guard: WL_GRAPHIC_GL_BLIT_DATA_H
