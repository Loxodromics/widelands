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
// Direction, in the normal's frame (FieldsToDraw::Field::normal, built by
// field_normal() in fields_to_draw.cc from kCos60/kSin60 -- an equilateral-hex
// layout, NOT map-pixel/screen space, where neighbours sit at +-45°). Elevation
// is kept at today's value: the old sun_vect = (0.577, -0.577, -0.577) in
// Field::set_brightness (src/logic/field.cc) is a light direction of
// (-0.577, +0.577, +0.577), horizontal magnitude 0.816 and z 0.577
// (35.3° elevation) -- this change moves the azimuth only. The rig's own 42.1°
// elevation (MEDIA_PIPELINE.md §2) is not transferable either way: Blender's
// ground plane is not the engine's 2:1-squashed field lattice, and the engine's
// z scale is kHeightFactor, not 1:1.
//
// The azimuth below is inferred, not measured directly, because field.cc's own
// note on the old vector concedes the geometry was "more guessed than thought
// about" -- there is no authoritative frame to appeal to here. What we do have
// is a checkable criterion: the on-screen bearing of the *brightest slope's
// uphill direction* should match the sprite shadows' measured bearing of
// atan(7.05 / 27.6) = 14.33° above the -x axis (centroids in
// Claude/MEDIA_PIPELINE.md §7). Converting that screen bearing into the
// normal's hex frame -- where a row step is 0.866 in hex-y against 0.5 in
// screen-y, a 1.732x stretch -- gives
// atan(tan(14.33°) / (0.5 / 0.866)) = 23.87°. Holding horizontal magnitude at
// the old light direction's 0.8165 and z at 0.5774:
// 0.8165 * (-cos 23.87°, -sin 23.87°), z = 0.5774. Checked against the
// criterion: this vector's brightest-slope bearing projects back to 14.35°
// on screen, against the target 14.33° (the naive same-frame reading used
// before this comment was rewritten missed it, landing at 8.39°). Flat
// ground's N*L stays 0.5774, same as the committed and pre-V2 vectors, so
// kSunColor/kAmbientColor below need no rescaling.
constexpr Vector3f kSunDirection = {-0.747f, -0.330f, 0.577f};

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
