// Render-side terrain lighting (V2, Claude/VISUAL_FIDELITY_RANKED.md §4.2). The
// direction/colour uniforms are derived and documented in
// src/graphic/gl/terrain_lighting.h; this is only the lambert term.
vec3 terrain_light(vec3 normal) {
	return u_ambient_color + u_sun_color * max(dot(normalize(normal), u_sun_direction), 0.0);
}
