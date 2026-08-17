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
// First calibrated against TERRAIN_NOISE.md §16's retired value-channel
// measurement (10.36% relative-field sd at kValueAmplitude = 0.40): with the
// sun's horizontal magnitude 0.8165 and flat ground at luma 0.901
// (terrain_lighting.h), a horizontal tilt t moves brightness by about
// 0.8165 * t / 0.901 = 0.906t, so a tilt RMS of 0.115 reproduces that same
// 10.4% sd. The composed bump gradient's RMS |tilt| at unit amplitude was
// measured in the scratchpad Python port (the same one that verified
// snoise_grad()'s analytic derivative against central differences) by
// sampling terrain_bump_gradient() at 200k random world positions and taking
// kBumpGradientToNormal * gradient's RMS magnitude: 4.4813, so RMS tilt r
// corresponds to kBumpAmplitude = r / 4.4813.
//
// That calibration point (r = 0.115, amplitude 0.0257) turned out to read as
// too strong once captured -- matching the retired channel's *brightness*
// swing is not the same as matching how loud a *normal perturbation* should
// look, since the two mechanisms respond to the same swing differently
// (lambert vs. a flat multiply). Judged down on a clean ladder (steppe00,
// view 1500,1200,1.0, each point a separate kBumpAmplitude + rebuild, tint
// unchanged so it is not a confound like scaling via terrain_noise_strength
// would be): r = 0.02 / 0.04 / 0.06 / 0.115 / 0.20, sd(delta-luma) against a
// no-bump render of 1.5 / 2.9 / 4.3 / 8.2 / 14.0 (roughly linear in r, as
// expected). Settled on r = 0.06 -- noticeable surface texture without
// reading as loud -- about half the retired-channel-matching point.
constexpr float kBumpAmplitude = 0.0134f;  // peak tilt, RMS 0.06 -- see above
constexpr float kTintAmplitude = 3.0f;    // peak warm/cool swing per unit tint field
constexpr float kWarpAmplitude = 0.0f;    // peak texture displacement in fields; disabled
                                           // 2026-08-16, was smearing the texture at the
                                           // higher noise frequencies - see Claude/TERRAIN_NOISE.md

#endif  // end of include guard: WL_GRAPHIC_GL_TERRAIN_NOISE_H
