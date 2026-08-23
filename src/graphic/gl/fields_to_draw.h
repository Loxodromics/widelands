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

#ifndef WL_GRAPHIC_GL_FIELDS_TO_DRAW_H
#define WL_GRAPHIC_GL_FIELDS_TO_DRAW_H

#include "base/vector.h"
#include "graphic/rendertarget.h"
#include "graphic/road_segments.h"
#include "logic/editor_game_base.h"
#include "logic/vision.h"
#include "logic/widelands_geometry.h"

// Helper struct that contains the data needed for drawing all fields.
class FieldsToDraw {
public:
	static constexpr int kInvalidIndex = std::numeric_limits<int>::min();

	struct Field {
		Widelands::Coords geometric_coords;  // geometric coordinates (i.e. map coordinates that can
		                                     // be out of bounds).
		Widelands::FCoords fcoords;  // The normalized coords and the field this is refering to.
		Vector2f gl_position = Vector2f::zero();  // GL Position of this field.

		// Surface pixel this will be plotted on.
		Vector2f surface_pixel = Vector2f::zero();

		bool obscured_by_slope;  // Whether this field is invisible due to an obstacle in front.

		// Rendertarget pixel this will be plotted on. This is only different by
		// the Rendertarget::get_rect().origin() of the view window.
		Vector2f rendertarget_pixel = Vector2f::zero();
		Vector2f texture_coords = Vector2f::zero();  // Texture coordinates.

		// Visibility factor only (fog of war), 1.0 for a fully visible field.
		// Slope shading used to be folded into this value; it now lives in
		// 'normal' instead and is applied by the terrain_light() shader term.
		// The initializer below only fires at construction, not per frame --
		// reset() is what makes this value correct each frame, and it must
		// assign it unconditionally (only the fog-of-war path is allowed to
		// override it afterwards). Without that assignment the field freezes
		// at whatever fog last wrote, which reads as initialised because of
		// this initializer but silently goes stale.
		float brightness = 1.f;

		// Surface normal in the equilateral-hex frame field_normal() builds it
		// in (fields_to_draw.cc) -- NOT map-pixel/screen space, where neighbours
		// sit at +-45° rather than the frame's kSin60 = 0.866 diagonal step (see
		// terrain_lighting.h and terrain_noise_params.glsl). Used for
		// render-side terrain lighting (V2, Claude/VISUAL_FIDELITY_RANKED.md
		// §4.2). Vector3f has no default constructor, hence the explicit
		// in-class initializer.
		Vector3f normal = Vector3f(0.f, 0.f, 1.f);

		// The next values are not necessarily the true data of this field, but
		// what the player should see. For example in fog of war we always draw
		// what we saw last.
		Widelands::RoadSegment road_e;
		Widelands::RoadSegment road_sw;
		Widelands::RoadSegment road_se;
		bool is_border;
		Widelands::VisibleState seeing;
		Widelands::Player* owner;  // can be nullptr.

		// Index of neighbors in this 'FieldsToDraw'. INVALID_INDEX if this
		// neighbor is not contained.
		int ln_index;
		int rn_index;
		int trn_index;
		int bln_index;
		int brn_index;

		[[nodiscard]] inline bool all_neighbors_valid() const {
			return ln_index != kInvalidIndex && rn_index != kInvalidIndex &&
			       trn_index != kInvalidIndex && bln_index != kInvalidIndex &&
			       brn_index != kInvalidIndex;
		}
	};

	// Reinitialize for the given view parameters.
	void reset(const Widelands::EditorGameBase& egbase,
	           const Vector2f& viewpoint,
	           float zoom,
	           RenderTarget* dst);

	// The number of fields to draw.
	[[nodiscard]] inline size_t size() const {
		return fields_.size();
	}

	// Get the field at 'index' which must be in bound.
	[[nodiscard]] inline const Field& at(const int index) const {
		return fields_.at(index);
	}

	// Returns a mutable field at 'index' which must be in bound.
	inline Field* mutable_field(const int index) {
		return &fields_[index];
	}

	// Calculates the index of the given field with ('fx', 'fy') being geometric
	// coordinates in the map. Returns INVALID_INDEX if this field is not in the
	// fields_to_draw.
	[[nodiscard]] inline int calculate_index(int fx, int fy) const {
		if (fx < min_fx_ || fx > max_fx_ || fy < min_fy_ || fy > max_fy_) {
			return kInvalidIndex;
		}
		return (fy - min_fy_) * w_ + (fx - min_fx_);
	}

	// The geometric field bounds this was reset() for. The shore distance field
	// derives its own grid window from them (shore_distance_field.h).
	[[nodiscard]] inline int min_fx() const {
		return min_fx_;
	}
	[[nodiscard]] inline int max_fx() const {
		return max_fx_;
	}
	[[nodiscard]] inline int min_fy() const {
		return min_fy_;
	}
	[[nodiscard]] inline int max_fy() const {
		return max_fy_;
	}

private:
	// Minimum and maximum field coordinates (geometric) to render. Can be negative.
	int min_fx_ = 0;
	int max_fx_ = 0;
	int min_fy_ = 0;
	int max_fy_ = 0;

	// Width and height in number of fields.
	int w_ = 0;
	int h_ = 0;

	std::vector<Field> fields_;
};

#endif  // end of include guard: WL_GRAPHIC_GL_FIELDS_TO_DRAW_H
