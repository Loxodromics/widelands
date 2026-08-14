#version 330

in vec4 var_color;

#define PI 3.141592653589793

out vec4 frag_color;

void main() {
	// Empirically found shading function that got consensus.
	float alpha = pow(cos(var_color.a * PI / 2.0), 1.5);
	frag_color = vec4(var_color.r, var_color.g, var_color.b, alpha);
}
