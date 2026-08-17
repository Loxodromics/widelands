// 2D simplex noise from https://github.com/ashima/webgl-noise
// Copyright (C) 2011 by Ashima Arts (Simplex noise)
// Copyright (C) 2011-2016 by Stefan Gustavson (Classic noise and others)
// Distributed under the MIT License.
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x * 34.0) + 1.0) * x); }

float snoise(vec2 v) {
	const vec4 C = vec4(0.211324865405187,   // (3.0 - sqrt(3.0)) / 6.0
	                    0.366025403784439,   // 0.5 * (sqrt(3.0) - 1.0)
	                   -0.577350269189626,   // -1.0 + 2.0 * C.x
	                    0.024390243902439);  // 1.0 / 41.0
	vec2 i = floor(v + dot(v, C.yy));
	vec2 x0 = v - i + dot(i, C.xx);
	vec2 i1;
	i1.x = step(x0.y, x0.x);
	i1.y = 1.0 - i1.x;
	vec4 x12 = x0.xyxy + C.xxzz;
	x12.xy -= i1;
	i = mod289(i);
	vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
	vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
	m = m * m;
	m = m * m;
	vec3 x = 2.0 * fract(p * C.www) - 1.0;
	vec3 h = abs(x) - 0.5;
	vec3 ox = floor(x + 0.5);
	vec3 a0 = x - ox;
	m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
	vec3 g;
	g.x = a0.x * x0.x + h.x * x0.y;
	g.yz = a0.yz * x12.xz + h.yz * x12.yw;
	return 130.0 * dot(m, g);
}

// Same field as snoise(), but also returns its analytic gradient: (value, d/dx,
// d/dy). Every corner offset (x0, x12.xy, x12.zw) differs from v only by a
// per-cell constant, so d(x_i)/dv is the identity for all three corners; the
// permutation-derived gradient direction (a0, h) and the Taylor-series norm
// factor are themselves constant within a cell. That makes the chain rule
// fall out of the intermediates snoise() already computes: with
// m_i = max(0.5 - |x_i|^2, 0) and g_i = grad_i . x_i,
//   d(m_i)/dv = -2 x_i           (chain rule on |x_i|^2)
//   d(g_i)/dv = grad_i           (x_i's Jacobian is the identity)
// so value = 130 * sum(norm_i * m_i^4 * g_i) differentiates to
//   grad(value) = 130 * sum(norm_i * (-8 m_i^3 g_i x_i + m_i^4 grad_i)).
// Where m_i is clamped to zero by the max() above, both terms vanish, so no
// branch is needed for continuity. Verified against central differences in
// the scratchpad Python port: ~1e-6 relative error over 5000 random points.
vec3 snoise_grad(vec2 v) {
	const vec4 C = vec4(0.211324865405187,
	                    0.366025403784439,
	                   -0.577350269189626,
	                    0.024390243902439);
	vec2 i = floor(v + dot(v, C.yy));
	vec2 x0 = v - i + dot(i, C.xx);
	vec2 i1;
	i1.x = step(x0.y, x0.x);
	i1.y = 1.0 - i1.x;
	vec4 x12 = x0.xyxy + C.xxzz;
	x12.xy -= i1;
	i = mod289(i);
	vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
	vec3 m0 = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
	vec3 x = 2.0 * fract(p * C.www) - 1.0;
	vec3 h = abs(x) - 0.5;
	vec3 ox = floor(x + 0.5);
	vec3 a0 = x - ox;
	vec3 norm = 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
	vec3 m0sq = m0 * m0;
	vec3 m4 = norm * m0sq * m0sq;
	vec3 m3 = norm * m0sq * m0;
	vec3 g;
	g.x = a0.x * x0.x + h.x * x0.y;
	g.yz = a0.yz * x12.xz + h.yz * x12.yw;
	float value = 130.0 * dot(m4, g);
	vec3 nm3g = m3 * g;
	vec2 grad = 130.0 * (-8.0 * (nm3g.x * x0 + nm3g.y * x12.xy + nm3g.z * x12.zw) +
	                     m4.x * vec2(a0.x, h.x) + m4.y * vec2(a0.y, h.y) + m4.z * vec2(a0.z, h.z));
	return vec3(value, grad);
}

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
// an unrelated part of the simplex domain (kTintOffset below) so it shares no
// structure with the bump field -- the same technique terrain_warp()
// uses below to keep its x/y displacement independent of the colour
// variation. Low octave dominates and gives tint its slow, broad "material"
// read (drier patches yellower, shaded growth cooler and greener -- see
// Claude/TERRAIN_NOISE.md §6); the high octave is a smaller admixture that
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
const vec2 kTintOffset = vec2(41.7, -13.2);  // arbitrary; only needs to land far from p=0

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
// Offset in texture coordinates. Two dedicated snoise calls so the x and y
// displacements are independent; reusing the existing octaves would couple
// the warp to the colour variation and make it anisotropic, because those
// are single scalars at fixed frequencies chosen for a different job.
vec2 terrain_warp(vec2 world_pos) {
	return u_warp_amplitude * vec2(
		snoise(world_pos * kWarpFrequency + vec2(17.3, 5.1)),
		snoise(world_pos * kWarpFrequency + vec2(-9.7, 23.4)));
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
