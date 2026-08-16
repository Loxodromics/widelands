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
// Both fields are weighted sums of the same three octaves, so the pair costs no
// extra snoise calls.
//
// The value, tint and warp amplitudes (u_value_amplitude, u_tint_amplitude,
// u_warp_amplitude) are uniforms, fed from C++ as members of the
// "per_program_state" block declared in terrain.fp/dither.fp. They scale with
// the terrain_noise_strength config option (see Claude/TERRAIN_NOISE.md).
//
// The tunable constants this function uses (octave frequencies, rotation,
// weights) live in terrain_noise_params.glsl, included ahead of this file.
vec2 terrain_fields(vec2 p) {
	mat2 rot = kOctaveRotation;
	float o1 = snoise(p * kOctave1Frequency);
	p = rot * p;
	float o2 = snoise(p * kOctave2Frequency);
	p = rot * p;
	float o3 = snoise(p * kOctave3Frequency);
	float value = (kValueWeight1 * o1 + kValueWeight2 * o2 + kValueWeight3 * o3) / kValueWeightSum;
	float tint = (kTintWeight1 * o1 + kTintWeight2 * o2 + kTintWeight3 * o3) / kTintWeightSum;
	return vec2(value, tint);
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

// Multiplier applied to the terrain texture colour, keyed on world position
// in field units (var_texture_position). The mix extrapolates for negative
// tint, giving a cool shift on one side and a warm shift on the other.
vec3 terrain_variation(vec2 world_pos) {
	vec2 fields = terrain_fields(world_pos);
	return (1.0 + u_value_amplitude * fields.x) *
	       mix(vec3(1.0), kWarmTint, u_tint_amplitude * fields.y);
}
