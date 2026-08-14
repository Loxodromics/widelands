#version 330

in vec3 var_color;

out vec4 frag_color;

void main() {
	frag_color = vec4(var_color, .8);
}
