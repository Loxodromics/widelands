#version 330

uniform sampler2D u_dither_texture;
uniform sampler2D u_terrain_texture;

layout(std140) uniform per_program_state {
	float u_z_value;
	float u_value_amplitude;
	float u_tint_amplitude;
	float u_warp_amplitude;
	vec2 u_texture_dimensions;
};

in float var_brightness;
in vec2 var_dither_texture_position;
in vec2 var_texture_position;
in vec2 var_texture_offset;

// TODO(sirver): This is a hack to make sure we are sampling inside of the
// terrain texture. This is a common problem with OpenGL and texture atlases.
#define MARGIN 1e-2

// The 300 es emitter hoists this above every declaration; do not move it up.
precision highp float;

#include "terrain_variation.glsl"

out vec4 frag_color;

void main() {
	vec2 texture_fract = clamp(
			fract(var_texture_position + terrain_warp(var_texture_position)),
			vec2(MARGIN, MARGIN),
			vec2(1. - MARGIN, 1. - MARGIN));
	vec4 clr = texture(u_terrain_texture, var_texture_offset + u_texture_dimensions * texture_fract);
	frag_color = vec4(
	   clr.rgb * var_brightness * terrain_variation(var_texture_position),
	   1. - texture(u_dither_texture, var_dither_texture_position).r);
}
