#version 330

uniform sampler2D u_terrain_texture;

layout(std140) uniform per_program_state {
	float u_z_value;
	float u_bump_amplitude;
	float u_tint_amplitude;
	float u_warp_amplitude;
	vec2 u_texture_dimensions;
	vec3 u_sun_direction;
	vec3 u_sun_color;
	vec3 u_ambient_color;
};

in float var_brightness;
in vec2 var_dither_params;
in float var_dither_ramp;
in vec3 var_normal;
in vec2 var_texture_offset;
in vec2 var_texture_position;

// TODO(sirver): This is a hack to make sure we are sampling inside of the
// terrain texture. This is a common problem with OpenGL and texture atlases.
#define MARGIN 1e-2

// The 300 es emitter hoists this above every declaration; do not move it up.
precision highp float;

#include "terrain_noise_params.glsl"
#include "terrain_variation.glsl"
#include "terrain_lighting.glsl"

out vec4 frag_color;

void main() {
	vec2 texture_fract = clamp(
			fract(var_texture_position + terrain_warp(var_texture_position)),
			vec2(MARGIN, MARGIN),
			vec2(1. - MARGIN, 1. - MARGIN));
	vec4 clr = texture(u_terrain_texture, var_texture_offset + u_texture_dimensions * texture_fract);
	// The shape octaves must not retract the band past the ramp's 1 end, so
	// they are clamped. At centre 0.90 they can reach -0.20 against 0.10 of
	// headroom, so this binds at the default per-terrain amplitude of 1.0 --
	// it shapes the normal look rather than guarding an extreme (see the band
	// budget in terrain_noise_params.glsl).
	//
	// Coverage is a hole density, not an opacity: edge.png kept 11% holes even
	// at its full end, and that residual perforation is what makes a terrain
	// boundary read as grain rather than as a drawn line.
	float shape = clamp(var_dither_params.x * dither_shape_field(var_texture_position),
	                    -(1.0 - kDitherCentre), kDitherCentre);
	float s = var_dither_ramp - kDitherCentre + shape;

	// The dissolve zone is deliberate width, not antialiasing; the fwidth term
	// is only a floor so it never gets thinner than a pixel when zoomed out.
	// var_dither_params.y is the per-terrain dither_softness (default 1.0),
	// which finally has a job here -- it scales this zone.
	float dissolve = max(kDitherDissolveWidth * var_dither_params.y,
	                     max(0.5 * fwidth(s), kDitherMinWidth));
	float coverage = kDitherMaxCoverage * smoothstep(-dissolve, dissolve, s);

	// Threshold each of the four surrounding cells, then blend bilinearly.
	// That is exactly what GL_LINEAR sampling of a 1-bit mask computes, and it
	// is what edge.png actually did -- set_dither_mask bound it LINEAR on both
	// filters. Thresholding a single cell instead leaves hard-edged cells,
	// which read as blocky salt-and-pepper rather than grain: the perforation
	// has to keep its soft sub-cell structure to pass for the old mask.
	vec2 cell = var_texture_position * kDitherGrainFrequency;
	vec2 cell_index = floor(cell);
	vec2 cell_fract = fract(cell);
	float grain_00 = step(dither_grain(cell_index), coverage);
	float grain_10 = step(dither_grain(cell_index + vec2(1.0, 0.0)), coverage);
	float grain_01 = step(dither_grain(cell_index + vec2(0.0, 1.0)), coverage);
	float grain_11 = step(dither_grain(cell_index + vec2(1.0, 1.0)), coverage);
	float dissolved = mix(mix(grain_00, grain_10, cell_fract.x),
	                      mix(grain_01, grain_11, cell_fract.x), cell_fract.y);

	// Below one pixel per cell the dissolve is sub-pixel and aliases, so fade
	// back to continuous coverage -- what mip filtering did for the mask.
	float pixels_per_cell = 1.0 / max(kDitherGrainFrequency *
	                                  max(fwidth(var_texture_position.x),
	                                      fwidth(var_texture_position.y)),
	                                  kDitherMinWidth);
	float grain_lod = smoothstep(kDitherGrainFadeMin, kDitherGrainFadeMax, pixels_per_cell);

	vec3 normal = terrain_bump_normal(var_normal, var_texture_position);
	frag_color = vec4(
	   clr.rgb * var_brightness * terrain_light(normal) * terrain_variation(var_texture_position),
	   mix(coverage, dissolved, grain_lod));
}
