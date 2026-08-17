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
//
// kBumpAmplitude (Claude/VISUAL_FIDELITY_RANKED.md §4.2 phase 2) replaces the
// retired kValueAmplitude: the noise now perturbs the lighting normal
// (terrain_bump_normal(), terrain_variation.glsl) instead of multiplying
// brightness. It scales the tilt terrain_bump_gradient() returns, in the same
// units terrain_noise_params.glsl's kBumpGradientToNormal produces -- not a
// brightness fraction, so it is not comparable to the old value by number.
//
// Calibrated, not guessed: TERRAIN_NOISE.md §16 measured the retired value
// channel at 10.36% relative-field sd at kValueAmplitude = 0.40. With the
// sun's horizontal magnitude 0.8165 and flat ground at luma 0.901
// (terrain_lighting.h), a horizontal tilt t moves brightness by about
// 0.8165 * t / 0.901 = 0.906t, so a tilt RMS of 0.115 reproduces that same
// 10.4% sd. The composed bump gradient's RMS |tilt| at unit amplitude was
// measured in the scratchpad Python port (the same one that verified
// snoise_grad()'s analytic derivative against central differences) by
// sampling terrain_bump_gradient() at 200k random world positions and taking
// kBumpGradientToNormal * gradient's RMS magnitude: 4.4813. kBumpAmplitude is
// then 0.115 / 4.4813. Ladder judged on captures: 0.06 / 0.115 / 0.20 RMS
// tilt (0.0134 / 0.0257 / 0.0446 in this constant's units) -- 0.115 is the
// "matches the retired channel" point and the expected answer; 0.20 is there
// to find where the surface starts to emboss.
constexpr float kBumpAmplitude = 0.0257f;  // peak tilt, RMS-calibrated -- see above
constexpr float kTintAmplitude = 3.0f;    // peak warm/cool swing per unit tint field
constexpr float kWarpAmplitude = 0.0f;    // peak texture displacement in fields; disabled
                                           // 2026-08-16, was smearing the texture at the
                                           // higher noise frequencies - see Claude/TERRAIN_NOISE.md

#endif  // end of include guard: WL_GRAPHIC_GL_TERRAIN_NOISE_H
