// 2D simplex noise from https://github.com/ashima/webgl-noise
// Copyright (C) 2011 by Ashima Arts (Simplex noise)
// Copyright (C) 2011-2016 by Stefan Gustavson (Classic noise and others)
// Distributed under the MIT License.
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x * 34.0) + 1.0) * x); }

float snoise(vec2 v) {
	const vec4 C = vec4(0.211324865405187,   // (3.0 - sqrt(3.0)) / 6.0
	                    0.366025403784439,   // 0.5 * (sqrt(3.0) - 1.0)
	                   -0.577350269189626,   // -1.0 + 2.0 * C.x
	                    0.024390243902439);  // 1.0 / 41.0
	vec2 i = floor(v + dot(v, C.yy));
	vec2 x0 = v - i + dot(i, C.xx);
	vec2 i1;
	i1.x = step(x0.y, x0.x);
	i1.y = 1.0 - i1.x;
	vec4 x12 = x0.xyxy + C.xxzz;
	x12.xy -= i1;
	i = mod289(i);
	vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
	vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
	m = m * m;
	m = m * m;
	vec3 x = 2.0 * fract(p * C.www) - 1.0;
	vec3 h = abs(x) - 0.5;
	vec3 ox = floor(x + 0.5);
	vec3 a0 = x - ox;
	m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
	vec3 g;
	g.x = a0.x * x0.x + h.x * x0.y;
	g.yz = a0.yz * x12.xz + h.yz * x12.yw;
	return 130.0 * dot(m, g);
}

// Terrain variation. The input is var_texture_position, which is world position
// in units of one field. See Claude/TERRAIN_NOISE.md.
//
// Value (brightness) and tint (warm/cool hue) are two independently sampled
// noise fields -- terrain_value_field() and terrain_tint_field() below --
// composed by terrain_variation(). They used to share the same three raw
// snoise() samples via two fixed linear-combination weightings (a cost-saving
// trick, see Claude/TERRAIN_NOISE.md §6 "Phase 1b"); Phase 1c replaced that
// with a dedicated field for tint, sampled at its own offset domain, since the
// shared scheme was only statistically decorrelated in aggregate, not
// independent per pixel.
//
// The value, tint and warp amplitudes (u_value_amplitude, u_tint_amplitude,
// u_warp_amplitude) are uniforms, fed from C++ as members of the
// "per_program_state" block declared in terrain.fp/dither.fp. They scale with
// the terrain_noise_strength config option (see Claude/TERRAIN_NOISE.md).
//
// The tunable constants these functions use (octave frequencies, rotation,
// weights) live in terrain_noise_params.glsl, included ahead of this file.

// Brightness ("value") field: 3-octave fBm, unchanged since Phase 1. Octave 3
// carries most of the weight and its frequency is pinned to the antiphase
// rule (see the comment above kOctave3Frequency in terrain_noise_params.glsl)
// because it also has to fight the terrain's 1-field UV repeat. Octaves 1 and
// 2 are free parameters tuned for regional/mid-scale grain.
float terrain_value_field(vec2 p) {
	mat2 rot = kOctaveRotation;
	float o1 = snoise(p * kOctave1Frequency);
	p = rot * p;
	float o2 = snoise(p * kOctave2Frequency);
	p = rot * p;
	float o3 = snoise(p * kOctave3Frequency);
	return (kValueWeight1 * o1 + kValueWeight2 * o2 + kValueWeight3 * o3) / kValueWeightSum;
}

// Warm/cool ("tint") field: an independently sampled 2-octave fBm, offset into
// an unrelated part of the simplex domain (kTintOffset below) so it shares no
// structure with terrain_value_field() -- the same technique terrain_warp()
// uses below to keep its x/y displacement independent of the colour
// variation. Low octave dominates and gives tint its slow, broad "material"
// read (drier patches yellower, shaded growth cooler and greener -- see
// Claude/TERRAIN_NOISE.md §6); the high octave is a smaller admixture that
// breaks up the low octave's smooth, rounded undulation. Neither carries the
// antiphase constraint that pins value's octave 3 and the warp frequency --
// tint has no repeat-breaking job (§16) -- so both are free parameters, tuned
// by eye, exactly like value's octaves 1 and 2.
//
// Because the two fields no longer share raw samples, there is no
// orthogonality constraint on the weights either: the old scheme needed a
// negative weight purely to keep its two projections of the same three
// numbers decorrelated. That requirement is gone -- these two weights just
// say how much each tint octave contributes.
const vec2 kTintOffset = vec2(41.7, -13.2);  // arbitrary; only needs to land far from p=0

float terrain_tint_field(vec2 p) {
	mat2 rot = kOctaveRotation;
	float lo = snoise(p * kTintFrequencyLow + kTintOffset);
	p = rot * p;
	float hi = snoise(p * kTintFrequencyHigh + kTintOffset);
	return (kTintWeightLow * lo + kTintWeightHigh * hi) / kTintWeightSum;
}

// Domain warp, applied to var_texture_position before the fract() in both
// shaders so the sampled texel moves instead of its brightness. See
// Claude/TERRAIN_NOISE.md §17. kWarpFrequency lives in terrain_noise_params.glsl.
//
// Offset in texture coordinates. Two dedicated snoise calls so the x and y
// displacements are independent; reusing the existing octaves would couple
// the warp to the colour variation and make it anisotropic, because those
// are single scalars at fixed frequencies chosen for a different job.
vec2 terrain_warp(vec2 world_pos) {
	return u_warp_amplitude * vec2(
		snoise(world_pos * kWarpFrequency + vec2(17.3, 5.1)),
		snoise(world_pos * kWarpFrequency + vec2(-9.7, 23.4)));
}

// Terrain-transition ("dither") field, sampled in dither.fp only. Two
// frequencies, two jobs: the low-frequency term displaces the whole boundary
// so it wanders across the map, the high-frequency term breaks it into a
// stipple. The amplitude is not global -- dither.vp supplies it per vertex
// from the overlay terrain's dither_amplitude (default 1.0), so only the
// frequencies stay constant here; a per-terrain *frequency* would put a seam
// wherever two overlay terrains meet. kDitherOffset parks the field in an
// unrelated part of the simplex domain so the border shape shares no
// structure with the terrain's colour variation. The constants live in
// terrain_noise_params.glsl.
float dither_field(vec2 p) {
	return kDitherShapeAmp * snoise(p * kDitherShapeFreq + kDitherOffset)
	     + kDitherStippleAmp * snoise(p * kDitherStippleFreq + kDitherOffset);
}

// Multiplier applied to the terrain texture colour, keyed on world position
// in field units (var_texture_position). The mix extrapolates for negative
// tint, giving a cool shift on one side and a warm shift on the other.
vec3 terrain_variation(vec2 world_pos) {
	float value = terrain_value_field(world_pos);
	float tint = terrain_tint_field(world_pos);
	return (1.0 + u_value_amplitude * value) *
	       mix(vec3(1.0), kWarmTint, u_tint_amplitude * tint);
}
