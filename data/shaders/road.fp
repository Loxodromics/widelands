#version 330

// Inputs.
in vec2 out_texture_position;
in float out_brightness;

uniform sampler2D u_texture;

out vec4 frag_color;

void main() {
	vec4 color = texture(u_texture, out_texture_position);
	color.rgb *= out_brightness;
	frag_color = color;
}
