#version 330

/* The WP-3 debug visualisation of the signed distance-to-shore field
 * (Claude/WATER.md §4.2). False colour only -- WP-6 turns this pass into the
 * real water wash. It exists so the field can be seen and trusted before any
 * styling depends on it.
 */

in vec2 var_texture_position;

layout(std140) uniform per_program_state {
	float u_z_value;
	float u_max_distance;
	float u_contour_spacing;
	float u_zero_band;
	// (cx0, cy0, 1/width, 1/height) of the distance field's grid window.
	vec4 u_grid;
};

uniform sampler2D u_shore_distance;

out vec4 frag_color;

const vec3 kShallowWater = vec3(0.35, 0.90, 0.95);
const vec3 kDeepWater = vec3(0.03, 0.10, 0.40);
const vec3 kShoreLand = vec3(0.62, 0.47, 0.30);
const vec3 kInlandLand = vec3(0.20, 0.13, 0.07);
const vec3 kZeroCrossing = vec3(1.00, 0.95, 0.20);
const vec3 kSaturated = vec3(1.00, 0.10, 0.80);
const float kContourPhaseOffset = 0.25;

void main() {
	/* attr_texture_position is (map_x / 64, -map_y / 64) (fields_to_draw.cc,
	 * where the y flip compensates for GL's upward y). One grid cell is 32 map
	 * pixels, hence the factor two into global cell coordinates. No further y
	 * flip is needed for the lookup: glTexImage2D's row 0 is v = 0, and the
	 * field is uploaded in increasing cy, i.e. increasing map y.
	 */
	vec2 cell = vec2(var_texture_position.x, -var_texture_position.y) * 2.0;
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
	vec2 uv = (cell - u_grid.xy + 0.5) * u_grid.zw;
	float shore = texture(u_shore_distance, uv).r;

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
}
