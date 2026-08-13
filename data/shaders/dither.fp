#version 120

uniform sampler2D u_dither_texture;
uniform sampler2D u_terrain_texture;
uniform vec2 u_texture_dimensions;

varying float var_brightness;
varying vec2 var_dither_texture_position;
varying vec2 var_texture_position;
varying vec2 var_texture_offset;

// TODO(sirver): This is a hack to make sure we are sampling inside of the
// terrain texture. This is a common problem with OpenGL and texture atlases.
#define MARGIN 1e-2

// TODO(philipp): The noise block below is a temporary duplicate of terrain.fp.
// GLSL 1.20 has no #include; a single-level textual include expansion in
// Program::build (gl/utils.cc) is the Phase 1b task that removes this copy.

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
float terrain_fbm(vec2 p) {
	// Rotate between octaves so the simplex lattice axes never line up.
	mat2 rot = mat2(0.80, 0.60, -0.60, 0.80);
	float sum = snoise(p * 0.09);
	p = rot * p;
	sum += 0.50 * snoise(p * 0.21);
	p = rot * p;
	sum += 1.20 * snoise(p * 0.55);
	return sum / 2.70;
}

const float kValueAmplitude = 0.07;

void main() {
	vec2 texture_fract = clamp(
			fract(var_texture_position),
			vec2(MARGIN, MARGIN),
			vec2(1. - MARGIN, 1. - MARGIN));
	vec4 clr = texture2D(u_terrain_texture, var_texture_offset + u_texture_dimensions * texture_fract);
	gl_FragColor = vec4(
	   clr.rgb * var_brightness * (1.0 + kValueAmplitude * terrain_fbm(var_texture_position)),
	   1. - texture2D(u_dither_texture, var_dither_texture_position).a);
}
