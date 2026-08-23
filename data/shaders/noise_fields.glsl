// Scrolling-noise primitives extracted from terrain_variation.glsl (Claude/WATER.md WP-6) so a
// shader with none of terrain.fp's per_program_state uniforms (water.vp/water.fp) can use them.
// GLSL resolves identifiers whether or not the function using them is called, so including
// terrain_variation.glsl wholesale would require u_time/u_bump_amplitude/u_tint_amplitude/
// u_warp_amplitude/u_cloud_amplitude all declared -- see that file's own header comment. Must be
// included after terrain_noise_params.glsl (frequency/velocity/offset constants) and
// simplex_noise.glsl (snoise); must not itself contain an #include (nested includes are rejected,
// see expand_includes() in gl/utils.cc).

// World-space scrolling-noise sample (Claude/WATER.md §4.9, WP-4/WP-6): world position scaled by
// a frequency (cycles per field), advanced by a velocity times time, and moved into its own part
// of the simplex domain by an offset so it decorrelates from every other field sampling the same
// clock. Callers apply their own response curve (clamp, remap, sign) to the result. 'time' was
// u_time here until WP-6, promoted to a parameter so water.vp/water.fp can call this without
// terrain.fp's uniform block -- see terrain_cloud_shadow() (terrain_variation.glsl) for the
// wrapper terrain.vp/dither.vp still call.
float scrolling_snoise(vec2 world_pos, float frequency, vec2 velocity, vec2 offset, float time) {
	return snoise(world_pos * frequency + time * velocity + offset);
}

// Cloud shadow (Claude/VISUAL_FIDELITY_RANKED.md §4.8): one scrolling_snoise() sample at regional
// scale, darkening the terrain where it is positive. See terrain_cloud_shadow()
// (terrain_variation.glsl) for the full rationale; this is that function's body with
// u_time/u_cloud_amplitude promoted to parameters ('time', 'amplitude') for the same reason as
// scrolling_snoise() above.
float cloud_shadow(vec2 world_pos, float time, float amplitude) {
	float n = scrolling_snoise(world_pos, kCloudFrequency, kCloudVelocity, kCloudOffset, time);
	return 1.0 - amplitude * max(n, 0.0);
}
