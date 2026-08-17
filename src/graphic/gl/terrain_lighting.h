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
 */

#ifndef WL_GRAPHIC_GL_TERRAIN_LIGHTING_H
#define WL_GRAPHIC_GL_TERRAIN_LIGHTING_H

#include "base/vector.h"

// The sun used for render-side terrain lighting (V2, Claude/VISUAL_FIDELITY_RANKED.md
// §4.2). These are C++ constants rather than GLSL constants because the sun is fed to
// the shaders as a uniform (see terrain_lighting.glsl), which is what a future
// day/night cycle needs; changing them still needs a rebuild, same as
// terrain_noise.h.
//
// Direction, in map-pixel space (+y is screen-down, same frame as
// FieldsToDraw::Field::normal): horizontal bearing comes from the two measured
// sprite shadow centroids in Claude/MEDIA_PIPELINE.md §7 (mean (+27.6, +7.05) px,
// normalised (0.969, 0.247)); the light points opposite the shadow, so
// (-0.969, -0.247). Elevation is deliberately kept at today's value: the old
// sun_vect = (0.577, -0.577, -0.577) in Field::set_brightness (src/logic/field.cc)
// is a light direction of (-0.577, +0.577, +0.577), horizontal magnitude 0.816 and
// z 0.577 (35.3° elevation) -- so this change moves the azimuth only.
// 0.816 * (-0.969, -0.247) with z = 0.577 gives the vector below. The rig's own
// 42.1° elevation (MEDIA_PIPELINE.md §2) is not transferable: Blender's ground
// plane is not the engine's 2:1-squashed field lattice, and the engine's z scale
// is kHeightFactor, not 1:1. The bearing is transferable because shadow offsets
// are measured in screen pixels, which map-pixel space already is.
constexpr Vector3f kSunDirection = {-0.791f, -0.202f, 0.577f};

// Colours, from the sprite rig table in MEDIA_PIPELINE.md §2: key (1.00, 0.91,
// 0.55) at energy 0.85, fill (0.76, 0.80, 0.99) at energy 0.65. Each normalised to
// unit Rec.709 luma, then scaled so flat ground lands where it lands today: the
// old field_brightness() mapped get_brightness() == 0 to
// (144 * 255 / 160) / 255 = 0.898, and the energies split that 0.9 between sun and
// ambient in the ratio 0.85 : 0.65. Sun contributes 0.9 * 0.567 = 0.510 at
// N*L = 0.577 (flat ground against kSunDirection), so its scale is 0.885; ambient
// is 0.9 * 0.433 = 0.390. Check: kSunColor * 0.577 + kAmbientColor =
// (0.934, 0.902, 0.790), luma 0.901.
constexpr Vector3f kSunColor = {0.980f, 0.892f, 0.539f};

// The rig's fill is a sun at 90° elevation (straight down), not a constant
// ambient. Modelling it as a constant is the simplification here: since terrain
// normals always have positive z the two only differ on steep slopes, and a
// constant is one dot product cheaper.
constexpr Vector3f kAmbientColor = {0.368f, 0.387f, 0.479f};

#endif  // end of include guard: WL_GRAPHIC_GL_TERRAIN_LIGHTING_H
