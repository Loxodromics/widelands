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

#ifndef WL_GRAPHIC_GL_TERRAIN_NOISE_H
#define WL_GRAPHIC_GL_TERRAIN_NOISE_H

// The default amplitudes of the terrain variation, previously const values in
// data/shaders/terrain_variation.glsl. WP-8 of the renderer modernization plan
// moved them to C++ so they can be fed to the shaders as per-program-state
// uniforms, scaled by the terrain_noise_strength config option (see
// Claude/TERRAIN_NOISE.md). Keep these in sync with that document: this header
// is now the single edit point for the three amplitudes.
constexpr float kValueAmplitude = 0.40f;  // peak brightness swing as a fraction
constexpr float kTintAmplitude = 3.0f;    // peak warm/cool swing per unit tint field
constexpr float kWarpAmplitude = 0.05f;   // peak texture displacement in fields

#endif  // end of include guard: WL_GRAPHIC_GL_TERRAIN_NOISE_H
