#version 330

/* The water pass (Claude/WATER.md §4.2, WP-6/WP-7). Two visualisations of the same signed
 * distance-to-shore field, chosen by u_debug: the real wash (default) composites a shallow-to-deep
 * colour ramp over the seabed the terrain pass now draws for water triangles, with its edge coming
 * from the warped field below; --water-debug (u_debug > 0.5) instead draws the WP-3 false-colour
 * view of the field itself, unmixed with any seabed, so the field can be seen and trusted
 * independently of how the wash reads it.
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

// The depth ramp (Claude/WATER.md WP-7): two linear segments through the shallow/mid/deep stops,
// keyed on 't' in [0, 1] -- callers pass depth_t = clamp(shore / u_max_distance, 0, 1), the same
// normalisation the debug view above already applies to |shore| for its own ramp.
vec3 water_depth_color(float t) {
	return mix(mix(kWaterColorShallow, kWaterColorMid, clamp(t / 0.5, 0.0, 1.0)),
	           kWaterColorDeep, clamp((t - 0.5) / 0.5, 0.0, 1.0));
}

void main() {
	/* attr_texture_position is (map_x / 64, -map_y / 64) (fields_to_draw.cc,
	 * where the y flip compensates for GL's upward y). One grid cell is 32 map
	 * pixels, hence the factor two into global cell coordinates. No further y
	 * flip is needed for the lookup: glTexImage2D's row 0 is v = 0, and the
	 * field is uploaded in increasing cy, i.e. increasing map y.
	 *
	 * The warp is applied here, to var_texture_position before the frame
	 * conversion, so its noise domain matches the one terrain.fp feeds
	 * terrain_variation.glsl (raw var_texture_position, map pixels / 64 with y
	 * negated) and the offset budget's decorrelation argument
	 * (terrain_noise_params.glsl) holds.
	 */
	vec2 world = var_texture_position + water_shore_warp(var_texture_position);
	vec2 cell = vec2(world.x, -world.y) * 2.0;
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
	float shore = texture(u_shore_distance, uv).r;

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
	 * currently bind at any zoom the game offers (terrain_noise_params.glsl) -- kept as a guard for
	 * when WP-9's foam band narrows the transition enough for it to matter. coverage is 0 deep
	 * inland, 1 in open water, and ramps smoothly through the coastline in between; frag_color.a
	 * scales it by the depth-driven opacity so kBlendAlpha's mix(dst, src, a) reads as
	 * mix(seabed_or_land, water, opacity(depth_t) * coverage) -- Claude/WATER.md §4.8's
	 * cancellation identity is why applying the cloud shadow here and in terrain.fp (on the seabed)
	 * composites correctly instead of double-darkening.
	 */
	float depth_t = clamp(shore / u_max_distance, 0.0, 1.0);
	vec3 color = water_depth_color(depth_t);
	float opacity = mix(kWaterOpacityShallow, kWaterOpacityDeep, depth_t);
	float w = max(kWaterEdgeWidth, 0.5 * fwidth(shore));
	float coverage = smoothstep(-w, w, shore);
	frag_color = vec4(color * var_brightness * var_cloud_shadow, opacity * coverage);
}
