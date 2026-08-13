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
// in units of one field, so a frequency of 0.09 means "patches roughly 11 fields
// across". See Claude/TERRAIN_NOISE.md.
//
// The 0.55 octave carries most of the amplitude deliberately. The repetition it
// has to break has a period of exactly one field, and a wave separates two
// points one period apart most strongly when its own wavelength is twice that -
// about 0.5 cycles per field. Higher frequencies are worse, not better: at ~1.0
// cycles per field adjacent tiles land back in phase.
//
// Both fields are weighted sums of the same three octaves, so the pair costs no
// extra snoise calls. The tint weighting is orthogonal to the value weighting
// (their dot product is 1.00 + 0.20 - 1.20 = 0), which keeps value and hue from
// trending together.
vec2 terrain_fields(vec2 p) {
	// Rotate between octaves so the simplex lattice axes never line up.
	mat2 rot = mat2(0.80, 0.60, -0.60, 0.80);
	float o1 = snoise(p * 0.09);
	p = rot * p;
	float o2 = snoise(p * 0.21);
	p = rot * p;
	float o3 = snoise(p * 0.55);
	// value: 1.00 / 0.50 / 1.20, normalised by 2.70   (unchanged from Phase 1)
	// tint:  1.00 / 0.40 / -1.00, normalised by 2.40  (orthogonal to the above)
	float value = (1.00 * o1 + 0.50 * o2 + 1.20 * o3) / 2.70;
	float tint = (1.00 * o1 + 0.40 * o2 - 1.00 * o3) / 2.40;
	return vec2(value, tint);
}

// Domain warp, applied to var_texture_position before the fract() in both
// shaders so the sampled texel moves instead of its brightness. See
// Claude/TERRAIN_NOISE.md §17.
//
// Warp frequency follows the same antiphase rule as octave 3 (§5): to differ
// most between two points one field apart, the wavelength wants to be two
// fields, i.e. ~0.5 cycles per field.
const float kWarpFrequency = 0.55;
const float kWarpAmplitude = 0.05;    // in fields; the knob to sweep

// Offset in texture coordinates. Two dedicated snoise calls so the x and y
// displacements are independent; reusing the existing octaves would couple
// the warp to the colour variation and make it anisotropic, because those
// are single scalars at fixed frequencies chosen for a different job.
vec2 terrain_warp(vec2 world_pos) {
	return kWarpAmplitude * vec2(
		snoise(world_pos * kWarpFrequency + vec2(17.3, 5.1)),
		snoise(world_pos * kWarpFrequency + vec2(-9.7, 23.4)));
}

const float kValueAmplitude = 0.40;
const vec3 kWarmTint = vec3(1.06, 1.00, 0.92);
// Chosen by capture over two ladders. Hue swing scales linearly; as mean
// |d(R-B)| in 8-bit codes, land / water: 1.5 -> 1.9/3.8, 3.0 -> 3.7/7.6,
// 5.0 -> 6.2/12.7, 8.0 -> 9.8/20.2. Below about 1 code is invisible, so 1.5
// sat near the quantization floor on land. Water takes roughly twice the land
// swing because it is heavily blue-weighted, which is what sets the ceiling.
// Clipping is not a constraint anywhere in that range (+0.24 points at worst).
const float kTintAmplitude = 3.0;

// Multiplier applied to the terrain texture colour, keyed on world position
// in field units (var_texture_position). The mix extrapolates for negative
// tint, giving a cool shift on one side and a warm shift on the other.
vec3 terrain_variation(vec2 world_pos) {
	vec2 fields = terrain_fields(world_pos);
	return (1.0 + kValueAmplitude * fields.x) * mix(vec3(1.0), kWarmTint, kTintAmplitude * fields.y);
}
