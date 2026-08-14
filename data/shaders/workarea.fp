#version 330

in vec4 var_overlay;

out vec4 frag_color;

void main() {
	frag_color = var_overlay;
}
