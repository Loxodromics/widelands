#version 330

uniform sampler2D u_texture;
uniform sampler2D u_mask;

in vec2 out_mask_texture_coordinate;
in vec2 out_texture_coordinate;
in vec4 out_blend;
in float out_program_flavor;
in vec3 out_light;

out vec4 frag_color;

void main() {
	vec4 texture_color = texture(u_texture, out_texture_coordinate);

	// See http://en.wikipedia.org/wiki/YUV.
	float luminance = dot(vec3(0.299, 0.587, 0.114), texture_color.rgb);

	if (out_program_flavor == 0.) {
		frag_color = vec4(texture_color.rgb, out_blend.a * texture_color.a);
	} else if (out_program_flavor == 1.) {
		frag_color = vec4(vec3(luminance) * out_blend.rgb, out_blend.a * texture_color.a);
	} else {
		vec4 mask_color = texture(u_mask, out_mask_texture_coordinate);
		float blend_influence = mask_color.r * mask_color.a;
		frag_color = vec4(
			mix(texture_color.rgb, out_blend.rgb * luminance, blend_influence),
				out_blend.a * texture_color.a);
	}
	// Field lighting (V3, Claude/VISUAL_FIDELITY_RANKED.md §4.3): white unless
	// the draw is scoped inside a RenderTarget::LightScope, so this is a no-op
	// everywhere except map-object sprites.
	frag_color.rgb *= out_light;
}
