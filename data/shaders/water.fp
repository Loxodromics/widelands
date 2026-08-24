#version 330

/* The water pass (Claude/WATER.md §4.2, WP-6/WP-7/WP-8/WP-8a/WP-9). Two visualisations of the same
 * signed distance-to-shore field, chosen by u_debug: the real wash (default) composites an animated
 * shallow-to-deep colour ramp over the seabed the terrain pass now draws for water triangles, with
 * its edge coming from the warped field below and a foam band running along the coast in the
 * shallowest zone (WP-9); --water-debug (u_debug > 0.5) instead draws the WP-3 false-colour view of
 * the field itself, unmixed with any seabed or wave motion, so the field can be seen and trusted
 * independently of how the wash reads it -- with the WP-9 shore frame tinted in where it is used,
 * as the instrument for judging frame stability.
 */

in vec2 var_texture_position;
in float var_brightness;
in float var_cloud_shadow;

layout(std140) uniform per_program_state {
	float u_z_value;
	float u_max_distance;
	float u_contour_spacing;
	float u_zero_band;
	float u_time;
	float u_cloud_amplitude;
	// Selects between the false-colour field visualisation (WP-3) and the real wash (WP-6):
	// a uniform rather than an enqueue gate, so --water-debug is a mode of this one pass, not
	// its on-switch (RenderQueue::set_water_debug(), render_queue.cc).
	float u_debug;
	// (cx0, cy0, 1/width, 1/height) of the distance field's grid window.
	vec4 u_grid;
};

uniform sampler2D u_shore_distance;

#include "terrain_noise_params.glsl"
#include "simplex_noise.glsl"
#include "noise_fields.glsl"
#include "simplex_noise_3d.glsl"

out vec4 frag_color;

const vec3 kShallowWater = vec3(0.35, 0.90, 0.95);
const vec3 kDeepWater = vec3(0.03, 0.10, 0.40);
const vec3 kShoreLand = vec3(0.62, 0.47, 0.30);
const vec3 kInlandLand = vec3(0.20, 0.13, 0.07);
const vec3 kZeroCrossing = vec3(1.00, 0.95, 0.20);
const vec3 kSaturated = vec3(1.00, 0.10, 0.80);
const float kContourPhaseOffset = 0.25;

// Domain warp for the shore distance sample (Claude/WATER.md §4.3, WP-5): displaces the position
// at which the field is sampled so its lattice-polygon contours read as an irregular coast
// rather than an offset polygon. See the amplitude/frequency bounds on kWaterWarpAmplitude in
// terrain_noise_params.glsl. Two independent snoise calls, same shape as terrain_warp()
// (terrain_variation.glsl), for an isotropic displacement.
//
// Evaluating snoise() per fragment here (rather than only in a pre-baked texture, as every other
// user of it does) reintroduces a source of panning drift the WP-3 field itself does not have:
// GPU barycentric interpolation of var_texture_position is not bit-exact between two camera
// positions for "the same" world point, and this warp is the first place that few-ULP jitter
// gets amplified into a full contour-line pixel flip, at the handful of contour pixels where the
// antialiasing threshold (fwidth() in main()) sits closest to a hard edge. Confirmed empirically:
// widening kGridMarginCells well past what the amplitude bound requires does not reduce it, so it
// is not a margin violation; cropping the differing pixels shows the contour tracing an identical
// path in both frames, wobbling by one pixel only at sharp bends. Order of the residual: <0.01%
// of pixels, magnitude comparable to 8-bit quantisation noise, confined to this debug pass's
// contour lines.
//
// WP-6 measured the same residual on the real wash's smooth coverage ramp, at a smaller order
// still: the panning gate (zoom 1.0, a 64 map-pixel shift, water_coast view) found max per-channel
// delta 1, 0.0021% of pixels differing horizontally and 0.0005% vertically -- smaller than the
// debug pass's own <0.01%, consistent with a smooth ramp being less sensitive to few-ULP position
// jitter than the debug view's near-hard fwidth() threshold, but not zero: the ramp's own
// half-width (kWaterEdgeWidth in practice -- fwidth(shore) does not bind at any zoom the game
// offers, see terrain_noise_params.glsl) is still crossed by the same jitter at the rare pixel
// sitting exactly on it.
vec2 water_shore_warp(vec2 world_pos) {
	return kWaterWarpAmplitude * vec2(
		snoise(world_pos * kWaterWarpFrequency + kWaterWarpOffset1),
		snoise(world_pos * kWaterWarpFrequency + kWaterWarpOffset2));
}

/* The warped-field lookup (the cell-coordinate / half-cell correction / C1 reconstruction / texture
 * fetch that used to sit inline in main(); factored out at WP-9 because the shore frame calls it
 * four more times). u_grid and u_shore_distance are file-scope uniforms, so this is callable from
 * any function. It samples at a *warped* position -- callers pass the position the warp produced.
 */
float shore_at(vec2 warped_pos) {
	/* One grid cell is 32 map pixels, hence the factor two into global cell
	 * coordinates. No further y flip is needed for the lookup: glTexImage2D's
	 * row 0 is v = 0, and the field is uploaded in increasing cy, i.e.
	 * increasing map y.
	 */
	vec2 cell = vec2(warped_pos.x, -warped_pos.y) * 2.0;
	/* Half a cell up, in v only. Cell (cx, cy) is centred on map pixel
	 * (32*cx, 32*cy), but the two triangles it describes both lie in
	 * y = [32*cy, 32*cy + 32] -- entirely *below* the centre, their centroids
	 * 21.3 and 10.7 map pixels down. The sample lattice therefore sits half a
	 * cell high against the geometry it describes, which measured as a 15.5 px
	 * offset of the zero crossing from the real waterline before this
	 * correction. x needs none: both triangles' centroids fall exactly on their
	 * cell centre there.
	 */
	cell.y -= 0.5;
	/* C1 reconstruction (§4.3): smoothstep the fractional part of the texel
	 * coordinate before the lerp, so the warped field is continuously
	 * differentiable rather than showing kinks at the 32 px cell grid.
	 * Applying the smoothstep to the sample *position* rather than four manual
	 * taps keeps this at one hardware bilinear fetch -- sampling at
	 * base + frac + 0.5 makes the hardware's own lerp weight exactly frac.
	 * With frac left as the identity this is algebraically the plain C0
	 * lookup, which makes the smoothstep an isolated A/B for the kink check
	 * (Claude/WATER.md WP-5): the two reconstructions differ everywhere inside
	 * a cell, not just at its boundary, so the A/B shows up as a systematic
	 * shift of every contour line's position rather than only at the 32 px
	 * grid -- diffed and confirmed to fall exactly on contour lines, not on
	 * flat colour, before this was trusted as "the reconstruction, not a bug".
	 */
	vec2 texel = cell - u_grid.xy;
	vec2 base = floor(texel);
	vec2 frac = texel - base;
	frac = frac * frac * (3.0 - 2.0 * frac);
	vec2 uv = (base + frac + 0.5) * u_grid.zw;
	return texture(u_shore_distance, uv).r;
}

/* Jacobian of water_shore_warp() (WP-9). The warp samples the two noise fields at
 * world_pos * kWaterWarpFrequency + kWaterWarpOffset, so by the chain rule its derivative is
 * kWaterWarpAmplitude * kWaterWarpFrequency times the two fields' gradients, laid out column by
 * column (column 0 = d/dx, column 1 = d/dy). The frame's central differences sit in *warped*
 * space (shore_at() takes the warped position), while the visible contour normal lives in
 * *world* space, and the two differ by this Jacobian -- which is far from identity here: with
 * A*f = 0.2 and Ashima simplex peaking near |grad| ~= 4.2, the off-diagonal terms reach ~0.84,
 * i.e. direction errors of tens of degrees if the correction is skipped. snoise_grad() also
 * computes the value water_shore_warp()'s snoise() calls produce; only the gradient components
 * are used here, so no consistency question with the main path arises.
 */
mat2 water_shore_warp_jacobian(vec2 world_pos) {
	vec3 n1 = snoise_grad(world_pos * kWaterWarpFrequency + kWaterWarpOffset1);
	vec3 n2 = snoise_grad(world_pos * kWaterWarpFrequency + kWaterWarpOffset2);
	float j = kWaterWarpAmplitude * kWaterWarpFrequency;
	return mat2(vec2(1.0 + j * n1.y, j * n2.y), vec2(j * n1.z, 1.0 + j * n2.z));
}

/* The shore frame (Claude/WATER.md §4.6, WP-9): the cross-shore unit normal (away from shore) and
 * the along-shore tangent, from central differences of the warped field at a fixed world-space
 * step of one grid cell (kFoamGradStep, in the var_texture_position units the field is expressed
 * in -- both axes are map pixels / 64, so the space is isotropic and |grad shore| ~= 1 for a
 * well-formed distance field). A fixed step is zoom-independent and well-conditioned; dFdx/dFdy
 * of shore was rejected because it is quad-constant (2x2 blockiness in the frame direction) and,
 * at high magnification, the per-pixel step of shore falls to the same order as the kR16F
 * texture's own quantisation.
 *
 * Stability notes. The chamfer produces exactly flat plateaus (WP-3's carried-forward
 * observation), so |grad| can legitimately reach 0; the epsilon guard handles it, and plateaus
 * sit far from shore, where the foam is already zero, so the (1, 0) fallback direction is never
 * visible. In a channel narrower than twice the band reach, both banks' foam overlaps and the
 * frame flips across the medial axis -- correct behaviour, not a defect: waves run in opposite
 * directions on opposite banks. Only the tangent's *sign* flips, and the elongation below is
 * symmetric in +/-t, so this pass is insensitive to it; WP-10's advection must be aware of it.
 *
 * There is no global along-shore coordinate: t = perp(grad s / |grad s|) is not curl-free in
 * general, so no scalar potential has it as its gradient, and the obvious approximation
 * dot(world, t) is numerically unusable here -- var_texture_position reaches ~150 on the
 * water_coast view and up to ~500 on a large map, so a frame rotation of order 1 rad/field turns
 * into hundreds of cycles of noise-argument change per field, i.e. white noise. The frame is
 * therefore used as a *direction*, not a coordinate: foam_noise() offsets a few taps along +/-t
 * by a local distance, with no dependence on |world|. WP-10 inherits this constraint: it must
 * advect the *sample position* along t by a bounded offset (flow-map style, with a cross-faded
 * wrap), not advance an unbounded along-shore coordinate.
 */
vec2 water_shore_frame(vec2 world_pos, vec2 warped_pos, out vec2 tangent) {
	vec2 g_warped =
	   vec2(shore_at(warped_pos + vec2(kFoamGradStep, 0.0)) - shore_at(warped_pos - vec2(kFoamGradStep, 0.0)),
	        shore_at(warped_pos + vec2(0.0, kFoamGradStep)) - shore_at(warped_pos - vec2(0.0, kFoamGradStep)));
	vec2 g_world = transpose(water_shore_warp_jacobian(world_pos)) * g_warped;
	float len = length(g_world);
	if (len < kFoamGradEpsilon) {
		tangent = vec2(0.0, -1.0);
		return vec2(1.0, 0.0);
	}
	vec2 normal = g_world / len;
	tangent = vec2(-normal.y, normal.x);
	return normal;
}

/* Foam breakup (WP-9): three simplex taps offset along the along-shore tangent by a local
 * distance, averaged -- this is what elongates the breakup along the coast rather than along the
 * tile grid. The taps span roughly one wavelength of the breakup field (2 * kFoamStretch ~=
 * 1 / kFoamFrequency), so features stretch along the shore by a factor of ~2-3. Sampled at
 * var_texture_position, unwarped, the same domain the wave field uses.
 */
float foam_noise(vec2 world_pos, vec2 tangent) {
	vec2 base = world_pos * kFoamFrequency + kFoamOffset;
	vec2 stretch = tangent * (kFoamStretch * kFoamFrequency);
	return (snoise(base - stretch) + snoise(base) + snoise(base + stretch)) / 3.0;
}

// The depth ramp (Claude/WATER.md WP-7): two linear segments through the shallow/mid/deep stops,
// keyed on 't' in [0, 1] -- callers pass depth_t = clamp(shore / u_max_distance, 0, 1), the same
// normalisation the debug view above already applies to |shore| for its own ramp.
vec3 water_depth_color(float t) {
	return mix(mix(kWaterColorShallow, kWaterColorMid, clamp(t / 0.5, 0.0, 1.0)),
	           kWaterColorDeep, clamp((t - 0.5) / 0.5, 0.0, 1.0));
}

/* Wave surface motion (Claude/WATER.md §4.6, WP-8a): three travelling wave trains plus a fine
 * detail layer. WP-8 built this as two scrolled simplex layers; smooth noise translating across
 * the surface has no crest structure and nothing in it ever forms or breaks, so it read as a
 * sliding texture at any amplitude. See terrain_noise_params.glsl for the constants' derivation
 * (the dispersion relation the three periods follow, the shore phase, the fades).
 */

/* One train: a sine in the direction 'dir', sharpened into a narrow crest and a wide trough.
 *
 * The wander term displaces the train along its own direction, so its crest lines bend; because
 * the field supplying it takes time as a third axis rather than a drift (see water_wave_field()),
 * the bends themselves form and dissolve in place.
 *
 * 'shore_phase' bends the crests near the coast (WP-8a's stand-in for refraction). It is added to
 * the phase, not multiplied into the wavenumber, and that distinction is the whole of it: a
 * depth-varying wavenumber multiplies against dot(dir, world_pos), i.e. against *absolute* map
 * position, so its distortion grows without bound with distance from the map origin -- at the far
 * side of a 190-field map a 50% wavenumber variation is already hundreds of radians, and the
 * surface reads as marbling rather than as waves (measured on Riverlands' bay, 172 fields out,
 * which is what put this comment here). It is also origin-dependent, which nothing else in this
 * shader is. A phase offset is bounded by construction and shifts crests by at most
 * kShorePhase / wave_number field widths.
 */
float wave_train(vec2 world_pos,
                 vec2 dir,
                 float wave_number,
                 float omega,
                 float wander,
                 float shore_phase,
                 float sharpness,
                 float wave_time) {
	float phase = wave_number * (dot(dir, world_pos) + kWaveWanderAmp * wander) -
	              omega * wave_time + shore_phase;
	return pow(0.5 + 0.5 * sin(phase), sharpness);
}

/* The mean of pow(0.5 + 0.5*sin(x), s) over a full period, subtracted from each train so that
 * sharpening changes the surface's texture without shifting its base tone -- and so that a
 * sharpness which fades with zoom (kCrestFade*Px) does not make the water brighten as the camera
 * pulls out.
 *
 * Exactly, that mean is binomial(2s, s) / 4^s = gamma(2s+1) / (gamma(s+1)^2 * 4^s), which GLSL
 * cannot evaluate. inversesqrt(pi*s + 0.858) fits it to better than 0.3% over s in [1, 3] (0.500 /
 * 0.374 / 0.339 / 0.312 against exact 0.500 / 0.375 / 0.340 / 0.313 at s = 1 / 2 / 2.5 / 3), which
 * is far inside the 8-bit quantisation of the swing it corrects.
 */
float wave_crest_mean(float sharpness) {
	return inversesqrt(3.14159265 * sharpness + 0.858);
}

/* Blends the three trains and the detail layer. Returns the signed field in 'wave' (roughly
 * [-1, 1], zero-mean) and the masked crest sum in 'crest' ([0, 1]), which the caller thresholds
 * for the crest highlight: a highlight belongs where the trains' crests coincide, so it reads as
 * sparkle at a few points rather than as an outline of every crest.
 */
void water_wave_field(vec2 world_pos,
                      float wave_time,
                      float depth_t,
                      float crest_fade,
                      float detail_fade,
                      out float wave,
                      out float crest) {
	float wander =
	   snoise3(vec3(world_pos * kWanderFrequency + kWanderOffset, wave_time * kWanderEvolve));
	float shore_phase = kShorePhase * (1.0 - depth_t);
	float sharpness = mix(1.0, kCrestSharpness, crest_fade);

	float a = wave_train(world_pos, kWaveDirA, kWaveNumberA, kWaveOmegaA, wander, shore_phase,
	                     sharpness, wave_time);
	float b = wave_train(world_pos, kWaveDirB, kWaveNumberB, kWaveOmegaB, wander, shore_phase,
	                     sharpness, wave_time);
	float c = wave_train(world_pos, kWaveDirC, kWaveNumberC, kWaveOmegaC, wander, shore_phase,
	                     sharpness, wave_time);

	float crest_sum = (kWaveWeightA * a + kWaveWeightB * b + kWaveWeightC * c) / kWaveWeightSum;

	float detail =
	   snoise3(vec3(world_pos * kDetailFrequency + kDetailOffset, wave_time * kDetailEvolve));
	wave = crest_sum - wave_crest_mean(sharpness) + kDetailWeight * detail_fade * detail;

	/* The crest sum's maxima sit on the beat lattice of three sines, which is regular enough that
	 * thresholding it directly for the highlight paints a visibly repeating grid of spots. Masking
	 * it with the detail field -- the one term in here with no period at all -- scatters them back
	 * onto the crests they belong to.
	 */
	crest = crest_sum * mix(kGlintMaskFloor, 1.0, 0.5 + 0.5 * detail);
}

void main() {
	/* attr_texture_position is (map_x / 64, -map_y / 64) (fields_to_draw.cc,
	 * where the y flip compensates for GL's upward y).
	 *
	 * The warp is applied here, to var_texture_position before the frame
	 * conversion, so its noise domain matches the one terrain.fp feeds
	 * terrain_variation.glsl (raw var_texture_position, map pixels / 64 with y
	 * negated) and the offset budget's decorrelation argument
	 * (terrain_noise_params.glsl) holds. shore_at() then does the lookup
	 * itself -- the half-cell correction and C1 reconstruction it applies are
	 * the ones this function used to carry inline.
	 */
	vec2 world = var_texture_position + water_shore_warp(var_texture_position);
	float shore = shore_at(world);

	if (u_debug > 0.5) {
		float magnitude = abs(shore);
		float ramp = clamp(magnitude / u_max_distance, 0.0, 1.0);
		// Light at the shore, dark away from it, on both sides, so the two ramps
		// read as one gradient across the waterline.
		vec3 color = shore < 0.0 ? mix(kShoreLand, kInlandLand, ramp) :
		                           mix(kShallowWater, kDeepWater, ramp);

		/* A contour every u_contour_spacing of |shore|, widened by the screen-space
		 * derivative so it stays roughly a pixel wide at every zoom rather than
		 * vanishing when zoomed out and fattening when zoomed in.
		 *
		 * The quarter-step phase offset matters. The chamfer produces exactly flat
		 * plateaus, and their values are sums of the two step weights, so a plateau
		 * frequently lands on an exact multiple of one field width. The level set of
		 * a flat region is that whole region, not a curve, and such a contour paints
		 * a solid cell-sized block instead of a line -- measured on the Riverlands
		 * lagoon, which is what put this offset here. Offsetting by a quarter field
		 * width puts every contour at a value no combination of the weights can hit.
		 * The floor under fwidth keeps smoothstep out of its undefined equal-edge
		 * case where the field is flat.
		 */
		float phase = magnitude / u_contour_spacing - kContourPhaseOffset;
		float to_contour = min(fract(phase), 1.0 - fract(phase));
		float contour = 1.0 - smoothstep(0.0, max(1.5 * fwidth(phase), 1e-5), to_contour);
		color *= mix(1.0, 0.25, contour);

		/* Saturation: the nearest seed lies beyond the clamp, so the field carries
		 * no distance information here. This is legitimate and common when zoomed
		 * out -- every point more than u_max_distance inland saturates -- so its
		 * presence is not a defect. What it is useful for is telling a real reading
		 * apart from a clamped one when judging the ramps, and for spotting
		 * unexplored terrain, which saturates whole regions at once.
		 */
		if (magnitude >= u_max_distance - 0.01) {
			color = kSaturated;
		}

		/* The WP-9 shore frame, tinted where the foam band will actually use it -- the wash's own
		 * rendered band, i.e. inside the early-out's land-side edge (-kWaterEdgeWidth, kFoamReach);
		 * shore < kFoamReach alone would cover every inland pixel, where the foam code never runs.
		 * R carries the cross-shore normal's x, G its y. This is the instrument for the "no
		 * flipping or singularities" criterion -- judged on the debug view rather than as an
		 * eyeball judgement on the foam itself. The zero-crossing band below is applied after
		 * this, so the waterline still reads on top of the frame.
		 */
		if (shore > -kWaterEdgeWidth && shore < kFoamReach) {
			vec2 tangent;
			vec2 normal = water_shore_frame(var_texture_position, world, tangent);
			color.r = 0.5 + 0.5 * normal.x;
			color.g = 0.5 + 0.5 * normal.y;
		}

		if (magnitude < u_zero_band) {
			color = kZeroCrossing;
		}

		// Let the terrain read through at about a quarter, so the contours can be
		// judged against the actual coastline underneath.
		frag_color = vec4(color, 0.75);
		return;
	}

	/* The real wash (WP-6 pass, WP-7 colour/opacity): a shallow-to-deep ramp composited over the
	 * seabed the terrain pass draws for water triangles (and over ordinary land, at zero coverage,
	 * everywhere else). The edge comes from the same warped field the debug view above visualises,
	 * read through a smooth ramp rather than a hard threshold -- see water_shore_warp()'s own
	 * comment for the (smaller, but not zero) panning residual this ramp still shows relative to
	 * the debug view's.
	 *
	 * depth_t drives both colour (water_depth_color()) and opacity: 0 at the shoreline and beyond,
	 * onto the land side, so the shallow stop and the lower shallow opacity are what's used through
	 * the whole coastline transition, letting the seabed genuinely show through there; 1 once
	 * |shore| reaches u_max_distance out in open water. This is independent of coverage below,
	 * which only antialiases the land/water edge itself (WATER.md WP-7).
	 *
	 * w is kWaterEdgeWidth in practice: the screen-space fwidth(shore) term next to it is the same
	 * floor-under-the-transition-width idiom dither.fp uses for kDitherMinWidth, but it does not
	 * currently bind at any zoom the game offers (terrain_noise_params.glsl) -- kept as a guard;
	 * WP-9's foam band is additive to this transition, not a narrowing of it, so the floor still
	 * does not bind. coverage is 0 deep
	 * inland, 1 in open water, and ramps smoothly through the coastline in between; frag_color.a
	 * scales it by the depth-driven opacity so kBlendAlpha's mix(dst, src, a) reads as
	 * mix(seabed_or_land, water, opacity(depth_t) * coverage) -- Claude/WATER.md §4.8's
	 * cancellation identity is why applying the cloud shadow here and in terrain.fp (on the seabed)
	 * composites correctly instead of double-darkening.
	 */
	/* Screen scale, for the zoom fades below. One field width is 1.0 in var_texture_position's x
	 * (fields_to_draw.cc), so the reciprocal of its screen-space derivative is pixels per field.
	 * Computed here, before the early-out, because a derivative taken after a conditional return
	 * would be in non-uniform control flow and is undefined there -- the same reason w is computed
	 * before the branch rather than next to coverage.
	 */
	float px_per_field = 1.0 / max(fwidth(var_texture_position.x), 1e-6);

	/* Early-out (WP-8): every triangle in frame reaches this shader, water and land alike
	 * (water_program.cc), so a fragment more than half a field width inland already contributes
	 * nothing -- coverage below is exactly 0 there. Skipping it here also skips the wave field for
	 * such fragments, which is the point: without this, every land pixel on screen would pay for
	 * the wave field's two snoise3 samples and three trains, none of which it can ever show. It
	 * does not skip water_shore_warp() above, whose two snoise calls every fragment still pays:
	 * this test needs 'shore', and 'shore' needs the warp. Bit-exact, not an approximation: under
	 * kBlendAlpha, mix(dst, src, 0) == dst regardless of src's colour, and w is computed early
	 * purely so this check can use it -- it is the same value coverage uses further down.
	 */
	float shore_fwidth = fwidth(shore);
	float w = max(kWaterEdgeWidth, 0.5 * shore_fwidth);
	if (shore <= -w) {
		frag_color = vec4(0.0);
		return;
	}

	float depth_t = clamp(shore / u_max_distance, 0.0, 1.0);
	vec3 color = water_depth_color(depth_t);

	/* Wave field (WP-8, reshaped at WP-8a): a colour swing along the ramp's own shallow/deep hue
	 * axis, scaled by depth (shallower water swings less, calming the surface near the shoreline
	 * where WP-9's foam will take over) rather than by a depth-varying speed, which would shear the
	 * field instead (see kWaveAmplitudeShallow/Deep's own comment). Opacity and coverage below are
	 * deliberately left keyed to the unperturbed depth_t/shore -- waves modulating how much seabed
	 * shows through would make the seabed itself shimmer, and moving the coastline edge here would
	 * pre-empt WP-9/WP-10's shore frame.
	 *
	 * wave_time wraps u_time again at a shorter, exact-divisor period -- see kWaveTimeWrapPeriod's
	 * own comment for the float-precision reasoning.
	 *
	 * The two fades keep the sharp end of the field off the aliasing floor when zoomed out: the
	 * crest sharpening relaxes toward a plain sine, and the detail layer goes away entirely. Both
	 * are expressed in screen pixels per wavelength, so they follow the camera rather than a zoom
	 * number (terrain_noise_params.glsl).
	 */
	float wave_time = mod(u_time, kWaveTimeWrapPeriod);
	float crest_fade = smoothstep(kCrestFadeMinPx, kCrestFadeMaxPx, px_per_field * kWaveLambdaC);
	float detail_fade =
	   smoothstep(kDetailFadeMinPx, kDetailFadeMaxPx, px_per_field / kDetailFrequency);
	float wave;
	float crest;
	water_wave_field(
	   var_texture_position, wave_time, depth_t, crest_fade, detail_fade, wave, crest);
	float wave_amplitude = mix(kWaveAmplitudeShallow, kWaveAmplitudeDeep, depth_t);
	color = clamp(color + kWaveColorSwing * wave_amplitude * wave, 0.0, 1.0);

	/* Crest highlight: the top of the crest sum only, so it lands where the trains coincide rather
	 * than along every crest, and gated off near the waterline (WP-9's foam belongs there) and when
	 * the crests get too small on screen to hold it. A partial, deliberately quiet take on §6's
	 * whitecaps -- see kGlintStart's comment.
	 */
	float glint = smoothstep(kGlintStart, 1.0, crest) *
	              smoothstep(kGlintDepthMin, kGlintDepthMax, depth_t) * crest_fade;
	color = clamp(color + kGlintColor * (kGlintWeight * glint), 0.0, 1.0);

	/* Foam band (WP-9, Claude/WATER.md §4.6): a band in the shallowest water zone, centred at
	 * kFoamCenter -- not falling off from zero. With kWaterEdgeWidth = 0.5 field widths the
	 * coverage transition is genuinely soft (coverage is only ~0.5 at shore = 0), so foam peaking
	 * at the waterline would be half transparent over sand and read as pale beach rather than
	 * white foam; kFoamCenter = 0.35 puts the peak where coverage is ~0.85, still visually at the
	 * contact. The band spans shore ~= [-0.15, 0.85] before the wobble -- essentially all
	 * water-side, leaving the land side entirely to WP-12's wet sand. kFoamWobble displaces the
	 * band in and out along the cross-shore axis, which is what produces the Appendix's "band
	 * boundaries wobble by roughly one to two tiles and are blotchy rather than contoured"
	 * without a second mechanism.
	 *
	 * The pixel floor (kFoamMinWidthPixels * fwidth(shore), the kDitherMinWidth idiom) is a guard
	 * -- kFoamHalfWidth binds at every zoom the game offers (measured at water_coast_zoom4, see
	 * WATER.md WP-9).
	 *
	 * Everything here -- the frame's four extra taps, the Jacobian's two snoise_grad() calls and
	 * the three breakup taps -- runs only inside the kFoamReach gate, so open water pays nothing.
	 * The gate is spatially coherent (whole screen regions of water are either near-shore or
	 * not), so the Jacobian's duplicate lattice work over the main path's snoise() calls is
	 * cheaper than upgrading every fragment's warp to snoise_grad() (measured, WATER.md WP-9).
	 */
	float foam = 0.0;
	if (shore < kFoamReach) {
		vec2 tangent;
		water_shore_frame(var_texture_position, world, tangent);
		float fn = foam_noise(var_texture_position, tangent);
		float shore_foam = shore + kFoamWobble * fn;
		float half_width = max(kFoamHalfWidth, kFoamMinWidthPixels * shore_fwidth);
		float d = abs(shore_foam - kFoamCenter) / half_width;
		foam = (1.0 - smoothstep(kFoamCore, 1.0, d)) * kFoamStrength;
	}

	float opacity = mix(kWaterOpacityShallow, kWaterOpacityDeep, depth_t);
	float coverage = smoothstep(-w, w, shore);

	/* Foam folds into color as a linear mix *before* the cloud-shadow multiply, and into opacity
	 * independently of the shadow -- Claude/WATER.md §4.8's cancellation identity is what breaks
	 * if foam went in additively. kFoamOpacity near 1 is what hides the seabed under the band
	 * and makes it read white rather than sand-tinted.
	 */
	color = mix(color, kFoamColor, foam);
	opacity = mix(opacity, kFoamOpacity, foam);
	frag_color = vec4(color * var_brightness * var_cloud_shadow, opacity * coverage);
}
