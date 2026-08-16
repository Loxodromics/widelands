#version 330

uniform sampler2D u_terrain_texture;

layout(std140) uniform per_program_state {
	float u_z_value;
	float u_value_amplitude;
	float u_tint_amplitude;
	float u_warp_amplitude;
	vec2 u_texture_dimensions;
};

in float var_brightness;
in vec2 var_dither_params;
in float var_dither_ramp;
in vec2 var_texture_offset;
in vec2 var_texture_position;

// TODO(sirver): This is a hack to make sure we are sampling inside of the
// terrain texture. This is a common problem with OpenGL and texture atlases.
#define MARGIN 1e-2

// The 300 es emitter hoists this above every declaration; do not move it up.
precision highp float;

#include "terrain_noise_params.glsl"
#include "terrain_variation.glsl"

out vec4 frag_color;

void main() {
	vec2 texture_fract = clamp(
			fract(var_texture_position + terrain_warp(var_texture_position)),
			vec2(MARGIN, MARGIN),
			vec2(1. - MARGIN, 1. - MARGIN));
	vec4 clr = texture(u_terrain_texture, var_texture_offset + u_texture_dimensions * texture_fract);
	// The shape octaves must not retract the band past the shared edge, so
	// they are clamped; at the default per-terrain amplitude of 1.0 the clamp
	// never fires, it only binds above ~1.4 (see the budget in
	// terrain_noise_params.glsl). The stipple is deliberately allowed to
	// overshoot -- that overshoot is the edge speckle.
	float shape = clamp(var_dither_params.x * dither_shape_field(var_texture_position),
	                    -(1.0 - kDitherCentre), kDitherCentre);
	float s = var_dither_ramp - kDitherCentre + shape
	        + var_dither_params.x * kDitherStippleAmp * dither_stipple(var_texture_position);
	// smoothstep(-w, w, s) is undefined at w == 0, reachable with
	// dither_softness 0 and zero fwidth on degenerate geometry.
	float w = max(kDitherSoftness * var_dither_params.y,
	              max(0.5 * fwidth(s), kDitherMinWidth));
	frag_color = vec4(
	   clr.rgb * var_brightness * terrain_variation(var_texture_position),
	   smoothstep(-w, w, s));
}
