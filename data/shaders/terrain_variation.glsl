// The simplex noise primitives (mod289, permute, snoise, snoise_grad) used to live here; WP-5
// moved them to simplex_noise.glsl so water.fp can include just those, uniform-free, without
// pulling in the u_time/u_warp_amplitude/u_bump_amplitude/u_tint_amplitude/u_cloud_amplitude
// uniforms this file's functions below reference (GLSL resolves identifiers whether or not the
// function using them is called, so including this file wholesale needs all of them declared).
// simplex_noise.glsl must be included before this file. WP-6 moved scrolling_snoise() itself, and
// the cloud-shadow body it feeds, out to noise_fields.glsl for the same reason: water.vp/water.fp
// have no per_program_state block to read u_time/u_cloud_amplitude from. noise_fields.glsl must be
// included before this file too.

// Terrain variation. The input is var_texture_position, which is world position
// in units of one field. See Claude/TERRAIN_NOISE.md.
//
// Two independently sampled noise fields drive three effects: the bump field
// (terrain_bump_gradient()/terrain_bump_normal() below) perturbs the surface
// normal, and the tint field (terrain_tint_field()) shifts hue, composed by
// terrain_variation(). A third, earlier field -- "value", a brightness
// multiply -- was retired here (Claude/VISUAL_FIDELITY_RANKED.md §4.2 phase 2)
// once real per-vertex normals existed: a brightness multiply does not react
// to the light and reads as a stain, where a perturbed normal is bright on the
// sun side and dark on the other and reads as surface. The bump field reuses
// the retired value field's octave frequencies (so it stays visually related
// to what it replaced) but not its weights -- see kBumpWeight1/2/3 in
// terrain_noise_params.glsl for why those had to be rederived rather than
// copied.
//
// The bump and tint amplitudes (u_bump_amplitude, u_tint_amplitude) are
// uniforms, fed from C++ as members of the "per_program_state" block declared
// in terrain.fp/dither.fp. They scale with the terrain_noise_strength config
// option (see Claude/TERRAIN_NOISE.md). u_warp_amplitude is the same kind of
// uniform, for terrain_warp() below.
//
// The tunable constants these functions use (octave frequencies, rotation,
// weights) live in terrain_noise_params.glsl, included ahead of this file.

// Analytic gradient of the 3-octave bump field, in var_texture_position's
// domain (world position in field units). Shares octave frequencies and
// kOctaveRotation with the retired value field so the two read as related,
// but not its weights -- see kBumpWeight1/2/3 in terrain_noise_params.glsl.
//
// The chain rule through the rotation has to be carried explicitly: octave n
// is sampled at rot^n . p, so d/dp of that composition is (rot^n)^T applied to
// the gradient snoise_grad() returns in the rotated frame -- which is
// `gradient * rot` in GLSL, since GLSL's vec*mat is defined as
// transpose(mat)*vec. Applying `* rot` once per rotation already taken chains
// correctly (octave 3 has been rotated twice, so it needs `* rot` twice).
// Getting this backwards (using `rot *` instead, or applying it fewer times
// than the position was rotated) rotates the bump lighting by ~37 degrees per
// missed/wrong step -- kOctaveRotation's own angle -- which does not look
// obviously wrong in a screenshot, only in a directional-response check.
vec2 terrain_bump_gradient(vec2 p) {
	mat2 rot = kOctaveRotation;
	vec3 n1 = snoise_grad(p * kOctave1Frequency);
	vec2 g1 = kOctave1Frequency * n1.yz;

	p = rot * p;
	vec3 n2 = snoise_grad(p * kOctave2Frequency);
	vec2 g2 = kOctave2Frequency * (n2.yz * rot);

	p = rot * p;
	vec3 n3 = snoise_grad(p * kOctave3Frequency);
	vec2 g3 = kOctave3Frequency * (n3.yz * rot * rot);

	return (kBumpWeight1 * g1 + kBumpWeight2 * g2 + kBumpWeight3 * g3) / kBumpWeightSum;
}

// Perturbs 'normal' (the interpolated per-vertex lighting normal, in
// field_normal()'s equilateral-hex frame -- see fields_to_draw.cc) by the bump
// field's gradient at 'world_pos' (var_texture_position's domain: map pixels /
// 64, y negated -- see fields_to_draw.cc). These are two different frames, and
// a gradient is a covector: it takes the *inverse* of the stretch a direction
// vector takes when changing frame. kBumpGradientToNormal
// (terrain_noise_params.glsl) folds together the domain's y-negation and that
// inverse-anisotropy correction; see the constant's own comment for the
// derivation. u_bump_amplitude absorbs the frame conversion's overall scale,
// so the normalize() below is only there to keep the amplitude's meaning
// stable -- terrain_light() (terrain_lighting.glsl) normalizes internally
// regardless.
vec3 terrain_bump_normal(vec3 normal, vec2 world_pos) {
	vec2 gradient = terrain_bump_gradient(world_pos);
	vec2 tilt = kBumpGradientToNormal * gradient;
	return normalize(normal + u_bump_amplitude * vec3(tilt, 0.0));
}

// Warm/cool ("tint") field: an independently sampled 2-octave fBm, offset into
// an unrelated part of the simplex domain (kTintOffset, terrain_noise_params.glsl)
// so it shares no structure with the bump field -- the same technique
// terrain_warp() uses below to keep its x/y displacement independent of the
// colour variation. Low octave dominates and gives tint its slow, broad
// "material" read (drier patches yellower, shaded growth cooler and greener --
// see Claude/TERRAIN_NOISE.md §6); the high octave is a smaller admixture that
// breaks up the low octave's smooth, rounded undulation. Neither carries the
// antiphase constraint that pins the bump field's octave 3 and the warp
// frequency -- tint has no repeat-breaking job (§16) -- so both are free
// parameters, tuned by eye, exactly like the bump field's octaves 1 and 2.
//
// Because the two fields no longer share raw samples, there is no
// orthogonality constraint on the weights either: the old scheme needed a
// negative weight purely to keep its two projections of the same three
// numbers decorrelated. That requirement is gone -- these two weights just
// say how much each tint octave contributes.
float terrain_tint_field(vec2 p) {
	mat2 rot = kOctaveRotation;
	float lo = snoise(p * kTintFrequencyLow + kTintOffset);
	p = rot * p;
	float hi = snoise(p * kTintFrequencyHigh + kTintOffset);
	return (kTintWeightLow * lo + kTintWeightHigh * hi) / kTintWeightSum;
}

// Domain warp, applied to var_texture_position before the fract() in both
// shaders so the sampled texel moves instead of its brightness. See
// Claude/TERRAIN_NOISE.md §17. kWarpFrequency lives in terrain_noise_params.glsl.
//
// Offset in texture coordinates (kWarpOffset1/2, terrain_noise_params.glsl). Two
// dedicated snoise calls so the x and y displacements are independent; reusing
// the existing octaves would couple the warp to the colour variation and make
// it anisotropic, because those are single scalars at fixed frequencies chosen
// for a different job.
vec2 terrain_warp(vec2 world_pos) {
	return u_warp_amplitude * vec2(
		snoise(world_pos * kWarpFrequency + kWarpOffset1),
		snoise(world_pos * kWarpFrequency + kWarpOffset2));
}

// Terrain-transition ("dither") shape field, sampled in dither.fp only. It
// displaces the whole boundary so it wanders across the map (bounded by the
// clamp at the call site, which stops it retracting past the overlay). The
// displacement is world-space, so it moves the boundary as one curve rather
// than per triangle. The amplitude is not
// global -- dither.vp supplies it per vertex from the overlay terrain's
// dither_amplitude (default 1.0), so only the frequencies stay constant
// here; a per-terrain *frequency* would put a seam wherever two overlay
// terrains meet. Rotating between the octaves (kOctaveRotation) and giving
// each noise call its own offset keeps the simplex lattices from lining up,
// in an unrelated part of the domain so the border shape shares no structure
// with the terrain's colour variation. The constants live in
// terrain_noise_params.glsl.
float dither_shape_field(vec2 p) {
	mat2 rot = kOctaveRotation;
	float o1 = kDitherShapeAmp * snoise(p * kDitherShapeFreq + kDitherShapeOffset);
	p = rot * p;
	float o2 = kDitherMidAmp * snoise(p * kDitherMidFreq + kDitherMidOffset);
	return o1 + o2;
}

// Dissolve grain. Quantised to a cell grid and hashed, reproducing what
// edge.png was: a 1-bit mask at roughly one texel per screen pixel at zoom 1.
// Quantising locks the pattern to the map, so it does not swim when the view
// scrolls, exactly as mask texels did not. The hash is uniform on [0,1), which
// makes kDitherMaxCoverage the hole density directly -- snoise is smooth and
// bell-distributed, and does neither.
//
// The sin-free form is deliberate: sin() based hashes lose their argument to
// float32 range reduction at these cell magnitudes, and GLSL 120 (the legacy
// 2.1 dialect, see emit_dialect in gl/utils.cc) has no integer ops to hash
// with. Wrapping the cell keeps the input small enough for fract() to stay
// well conditioned on large maps; the period is 4096 cells, far past anything
// on screen.
float dither_grain(vec2 cell) {
	vec3 p3 = fract(vec3(mod(cell, kDitherGrainPeriod).xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

// Multiplier applied to the terrain texture colour, keyed on world position in
// field units (var_texture_position). The mix extrapolates for negative tint,
// giving a cool shift on one side and a warm shift on the other. This is the
// tint axis only now -- the retired value (brightness) axis is
// terrain_bump_normal() above instead, applied to the lighting normal rather
// than the colour.
vec3 terrain_variation(vec2 world_pos) {
	float tint = terrain_tint_field(world_pos);
	return mix(vec3(1.0), kWarmTint, u_tint_amplitude * tint);
}

// Cloud shadow (Claude/VISUAL_FIDELITY_RANKED.md §4.8): one scrolling_snoise()
// sample (noise_fields.glsl) at regional scale, scrolled by u_time (gametime in
// seconds, wrapped -- see kCloudTimeWrapPeriod in terrain_noise.h) and
// darkening the terrain where it is positive. max(n, 0.0) means the shadow
// only ever subtracts light, never brightens -- physically what a cloud does
// -- so roughly half the map is unshadowed at any instant. The frequency and
// drift live in terrain_noise_params.glsl; u_cloud_amplitude is a uniform from
// C++ and, like u_time, is deliberately not scaled by the terrain noise
// strength option (see terrain_noise.h). This wrapper exists so terrain.vp/
// dither.vp can keep calling it unchanged; cloud_shadow() itself (noise_fields.glsl)
// takes time/amplitude as parameters so water.vp can call it too (WP-6).
//
// Evaluated in the vertex shader (terrain.vp/dither.vp) and interpolated to
// the fragment stage as var_cloud_shadow, not recomputed per fragment. It
// depends only on var_texture_position (constant per field) and u_time
// (constant per frame), so it is constant across each field; at kCloudFrequency
// it varies by ~2-4% of its range across one triangle, so linear interpolation
// is visually exact. The max(n, 0.0) clamp applies before interpolation; the
// resulting sub-pixel error at the clamp contour is invisible at this scale.
float terrain_cloud_shadow(vec2 world_pos) {
	return cloud_shadow(world_pos, u_time, u_cloud_amplitude);
}
