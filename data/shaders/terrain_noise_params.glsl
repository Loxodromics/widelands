// Tunable constants for terrain noise. Edited and re-run with no rebuild
// (unlike the three amplitudes in src/graphic/gl/terrain_noise.h, which are
// uniforms and need one). See Claude/TERRAIN_NOISE.md.

// The offset budget (Claude/WATER.md §4.9, WP-4). Every noise field below samples the same
// simplex domain, so two fields at the same offset (or at offsets a simple multiple/rotation of
// each other) would beat against one another and read as correlated rather than as independent
// texture. Each field's offset is chosen to land far from every other one listed here, in an
// otherwise arbitrary part of the domain. This is the one place to check before adding a new
// field's offset -- do not invent one by only looking at a neighbouring constant's comment.
//
// field           frequency        velocity (scroll)         offset
// bump            kOctave1/2/3Frequency   static             none -- this is the field every
//                                                             other one below decorrelates from
// tint            kTintFrequencyLow/High  static             kTintOffset
// warp            kWarpFrequency          static             kWarpOffset1, kWarpOffset2
// cloud shadow    kCloudFrequency         kCloudVelocity      kCloudOffset
// dither shape    kDitherShapeFreq        static              kDitherShapeOffset
// dither mid      kDitherMidFreq          static              kDitherMidOffset
// dither grain    kDitherGrainFrequency   static              none -- cell-hashed, not simplex,
//                                                             so it cannot correlate with the
//                                                             fields above by construction
// water warp      kWaterWarpFrequency     static             kWaterWarpOffset1, kWaterWarpOffset2
//
// Cloud shadow is the only scrolling field today; scrolling_snoise() (terrain_variation.glsl) is
// the shared helper for any later one (WP-8's wave layers). A new field goes in this table with
// its own row when it lands.

// Rotate between octaves so the simplex lattice axes never line up.
const mat2 kOctaveRotation = mat2(0.80, 0.60, -0.60, 0.80);

// Frequencies are in cycles per field. Octave 3 was originally tuned to carry
// most of the amplitude of the brightness ("value") field that has since been
// retired (Claude/VISUAL_FIDELITY_RANKED.md §4.2 phase 2); its frequency is
// still not a free parameter, but the job it is pinned against has changed --
// see the paragraph below. Antiphase points so far: 0.5 (original) -> 1.5 ->
// 2.5 -> 3.5 -> 4.5 -> 5.5 -> 6.5 -> 7.5 -> 8.5 (2026-08-16, current -- this
// comment previously lagged the code at "6.5", corrected 2026-08-17). At 8.5
// the wavelength (0.118 fields, ~1.9 screen px at max zoom-out) is close to
// the ~4 px aliasing floor from §9 - unverified beyond "doesn't look obviously
// wrong in a screenshot crop"; no FFT/autocorrelation check has been done at
// this or the previous points. Octaves 1 and 2 carry no such constraint -
// they are free parameters tuned by eye for regional/mid-scale texture grain.
//
// What the antiphase pin is *for*, post-retirement: TERRAIN_NOISE.md §16
// measured that the value field this octave was originally aimed at could
// never break the texture's one-field repeat at any amplitude, at any
// frequency on this ladder -- a brightness multiply cannot change the
// sampled texel. The pin is kept anyway, for two reasons that have nothing to
// do with repetition: it keeps the bump field (which took over this
// frequency, see kBumpWeight3 below) at the same tonal-contrast scale the
// value field used, and it keeps kWarpFrequency (still the one mechanism that
// does touch the sampled texel, see §17) in sync with it, per the comment on
// kWarpFrequency below.
const float kOctave1Frequency = 1.45;
const float kOctave2Frequency = 4.05;
const float kOctave3Frequency = 8.5;

// Bump field (data/shaders/terrain_variation.glsl, terrain_bump_gradient()):
// the retired value field's octave weights (1.00 / 0.50 / 1.20) cannot be
// reused verbatim, because a weight's contribution to *brightness* is just
// itself, but its contribution to *tilt* is weight times frequency -- at the
// value weights, octave 3 alone would carry 75% of the bump (1.2*8.5 against
// a 1.45:4.05:8.5 tilt ratio of 1.45:2.03:10.2). These weights instead put
// the three octaves' *tilt* contributions back into the value field's
// 1:0.5:1.2 *brightness* ratio: kValueWeight_n / kOctaveNFrequency, rescaled
// so octave 1 is 1.0 -- (1.0/1.45, 0.5/4.05, 1.2/8.5) / (1.0/1.45) =
// (1.00, 0.18, 0.20). See Claude/VISUAL_FIDELITY_RANKED.md §4.2 phase 2 for
// the full derivation.
const float kBumpWeight1 = 1.00;
const float kBumpWeight2 = 0.18;
const float kBumpWeight3 = 0.20;
const float kBumpWeightSum = 1.38;

// Converts the bump field's gradient (terrain_bump_gradient(), in
// var_texture_position's domain) into a tilt in field_normal()'s
// equilateral-hex frame (fields_to_draw.cc). Two effects, folded into one
// constant:
//
// - var_texture_position negates y relative to map-pixel space
//   (fields_to_draw.cc), which the x component of this constant does not
//   touch and the y component's sign captures.
// - The hex frame is anisotropic: for a pure x-gradient of c height units per
//   field, field_normal() returns normal.x/normal.z = -0.234c against a
//   geometrically correct -0.0781c (a factor of 3.0); for a pure y-gradient,
//   -0.135c against the same -0.0781c (a factor of 1.732), so the hex frame's
//   y axis is compressed by 1.732/3.0 = 0.577 relative to its x axis. A
//   gradient is a covector, so converting it into this frame takes the
//   *inverse* of the stretch a direction vector would take -- the opposite
//   correction from the one terrain_lighting.h applies to the sun direction
//   (which multiplies screen-y by 1.732 to enter this same frame; a gradient
//   divides by it instead). The overall factor of 3.0 is absorbed into
//   u_bump_amplitude (terrain_noise.h) rather than carried here.
const vec2 kBumpGradientToNormal = vec2(-1.0, 0.577);

// tint: independent 2-octave field (terrain_tint_field(), terrain_variation.glsl).
// No orthogonality constraint -- these weights just set each octave's share.
// Low frequency dominates for tint's slow, broad "material" read; high
// frequency is a smaller admixture breaking up the low octave's smooth
// undulation. Neither is antiphase-constrained -- free parameters, tuned by
// eye. STARTING POINT, pending tuning (2026-08-16).
const float kTintFrequencyLow = 0.35;
const float kTintFrequencyHigh = 5.5;
const float kTintWeightLow = 1.0;
const float kTintWeightHigh = 0.35;
const float kTintWeightSum = 1.35;
const vec2 kTintOffset = vec2(41.7, -13.2);  // arbitrary; only needs to land far from p=0

// Warp frequency follows the same antiphase rule as octave 3 (see above) and
// tracks it to the same value, 8.5 as of 2026-08-16 (this comment previously
// lagged the code at "6.5", corrected 2026-08-17). Currently inert:
// kWarpAmplitude (terrain_noise.h) is 0 as of the same date - domain warping
// was disabled because it smeared the texture at these higher frequencies.
// Kept in sync in case the amplitude is ever raised again. See the aliasing
// note above octave 3 if it is.
const float kWarpFrequency = 8.5;

// Offset in texture coordinates for terrain_warp()'s two independent snoise calls
// (terrain_variation.glsl) -- arbitrary; only need to differ from each other and land far from
// p=0, same technique as kTintOffset above.
const vec2 kWarpOffset1 = vec2(17.3, 5.1);
const vec2 kWarpOffset2 = vec2(-9.7, 23.4);

// Cloud shadow (terrain_cloud_shadow(), terrain_variation.glsl): one snoise octave at regional
// scale, scrolled by u_time (kCloudVelocity), darkening the terrain where it is positive. The
// frequency is an order of magnitude below kOctave1Frequency, so the shadow reads as weather
// rather than as surface grain. kCloudOffset decorrelates it from the bump/tint/dither fields.
// STARTING POINT, pending tuning -- see Claude/VISUAL_FIDELITY_RANKED.md §4.8.
const float kCloudFrequency = 0.06;
const vec2 kCloudVelocity = vec2(0.064, 0.032);  // cycles/second, frequency-domain drift
const vec2 kCloudOffset = vec2(97.3, -61.9);     // arbitrary; must land far from the other fields

// Chosen by capture over two ladders. Hue swing scales linearly; as mean
// |d(R-B)| in 8-bit codes, land / water: 1.5 -> 1.9/3.8, 3.0 -> 3.7/7.6,
// 5.0 -> 6.2/12.7, 8.0 -> 9.8/20.2. Below about 1 code is invisible, so 1.5
// sat near the quantization floor on land. Water takes roughly twice the land
// swing because it is heavily blue-weighted, which is what sets the ceiling.
// Clipping is not a constraint anywhere in that range (+0.24 points at worst).
const vec3 kWarmTint = vec3(1.06, 1.00, 0.92);

// Dither-band (terrain-transition) parameters, see
// Claude/VISUAL_FIDELITY_RANKED.md §4.1. Replaces the stretched 1-bit edge.png
// mask with a world-space noise threshold.
//
// Ramp units: DitherProgram sets the ramp to 1 at a triangle vertex incident
// to the overlay terrain and 0 elsewhere, so 1 means "at the overlay" and 0
// "as far from it as this triangle goes". Because incidence belongs to the
// vertex, not the triangle, two triangles sharing an edge agree along it and
// the coverage field is continuous across the whole mesh -- the ramp is a
// crude distance-to-the-terrain-boundary, not a per-triangle gradient. The
// shape octave lets the boundary wander regionally, the mid octave gives it
// local irregularity.
//
// Band budget: at centre 0.90 there is only 0.10 of headroom to the ramp's
// 1 end, less than the shape octaves can produce (0.06 + 0.14 = 0.20), so
// dither.fp's clamp to [-(1 - kDitherCentre), kDitherCentre] is load-bearing
// rather than a safety net -- it binds wherever the shape field drops below
// -0.10 and holds the band at the overlay instead of letting it retract past.
// Density there falls to half the ceiling, which reads as extra speckle.
//
// The constants below are traced to edge.png's measured profile: 89%
// coverage at the shared edge, zero by ramp 0.75, 50% point at ramp 0.86.
// That coverage figure is a hole density, not an opacity -- edge.png was a
// 1-bit mask stretched over the triangle at roughly one texel per screen
// pixel at zoom 1, so even its "full" end kept 11% holes. dither_grain()
// (terrain_variation.glsl) reproduces that density with a uniform hash, so
// kDitherMaxCoverage sets it directly and dither.fp applies it as a threshold
// rather than an opacity multiply. The perforation is what makes the boundary
// read as Settlers-2 grain instead of a clean vector edge.
//
// WRONG, kept as a warning -- this paragraph used to claim kDitherCentre has a
// hard floor protecting a terrain one triangle wide (a beach between grass and
// water), on the grounds that the strip keeps 2t - 1 of its width, so a third
// of it at centre 0.90. That argument assumes the ramp still varies across the
// strip. Since dither geometry became vertex-incident, it does not: every
// vertex of a one-triangle-wide strip touches the overlay, the ramp is 1 over
// the whole triangle, and s = ramp - kDitherCentre + shape is then bounded
// below by ramp - 1 = 0 by dither.fp's clamp. Coverage there cannot fall under
// half the ceiling at ANY value of kDitherCentre, kDitherShapeAmp or
// kDitherMidAmp, and the strip is erased rather than thinned -- a beach spit on
// coast01 loses ~80% of its pixels.
//
// This is an OPEN defect; kDitherCentre cannot fix it and tuning it in the hope
// of doing so will only trade the erasure for something else. Two attempted
// fixes were reverted, the second (a per-vertex ramp ceiling) because it put
// hard triangular patches of bare base terrain across every irregular boundary.
// Read Claude/VISUAL_FIDELITY_RANKED.md section 4.1 before trying a third.
// kDitherCentre itself is a free aesthetic parameter.
//
// kDitherGrainFadeMin/Max: var_texture_position is map pixels / 64 on both
// axes (fields_to_draw.cc:147-148) and kDitherGrainFrequency is 64, so
// pixels_per_cell is exactly 1 / zoom -- one grain cell per screen pixel at
// the default zoom (MapView::reset_zoom()). The fade must therefore hold full
// grain at 1.0 and reach zero by 0.5 (zoom 2), which needs two independent
// endpoints. A clamp(pixels_per_cell - k, 0, 1) ties the fade's start to its
// width and cannot express that: k = 2.0 fades the grain out at every zoom the
// game offers, k = 0.25 still leaves a quarter of it at zoom 2.
const float kDitherCentre = 0.90;
const float kDitherShapeAmp = 0.06;
const float kDitherShapeFreq = 0.20;
const float kDitherMidAmp = 0.14;
const float kDitherMidFreq = 2.50;
const float kDitherDissolveWidth  = 0.09;   // half-width of the dissolve zone, in ramp units
const float kDitherMaxCoverage    = 0.89;   // ceiling: 11% holes even at the shared edge
const float kDitherGrainFrequency = 64.0;   // cells per field ~= 1 px per cell at zoom 1
const float kDitherGrainFadeMin   = 0.5;    // no grain at or below this many px per cell
const float kDitherGrainFadeMax   = 1.0;    // full grain at or above it (== zoom 1.0)
const float kDitherGrainPeriod    = 4096.0; // hash input wrap, for float32 conditioning

// Floor under the dissolve zone width and the pixels-per-cell fade term
// (dither.fp): smoothstep(-w, w, s) is undefined at w == 0, reachable with
// zero fwidth on degenerate geometry. Small enough to never influence the
// rendered band.
const float kDitherMinWidth = 1e-4;

// Arbitrary; only needs to land far from p=0 and from each other so the
// border shape shares no structure with the bump/tint/warp fields or with
// itself (same technique as kTintOffset).
const vec2 kDitherShapeOffset = vec2(73.1, -41.8);
const vec2 kDitherMidOffset = vec2(-55.3, 18.2);

// Water shore-distance domain warp (water_shore_warp(), water.fp; Claude/WATER.md §4.3, WP-5).
// Displaces the position at which the shore distance field is sampled, so the field's contours
// -- lattice polygons at source, see §4.3 -- read as an irregular coast instead of an offset
// polygon. Static (no u_time term): the warp only needs to break up the lattice shape, not
// animate, so water.fp does not need u_time in its uniform block for this. Two independent
// snoise calls, same technique as terrain_warp() above, so the x/y displacement is isotropic.
//
// The amplitude is bounded on two independent sides, both against the frequency:
//  - kGridMarginCells (shore_distance_field.h) gives the field 13 cells (~1 field width) of
//    slack past kMaxShoreDistance before WP-3's translation-invariance argument breaks down, so
//    kWaterWarpAmplitude must stay under 1.0 field width or the field at the frame edge becomes
//    view-dependent again and the panning gate starts failing. Raise kGridMarginCells (a
//    one-line change, +7% cells at 1080p/zoom 1, 29 ns/cell) rather than silently exceeding this.
//  - Fold-over: the warp map p -> p + A*n(f*p) stops being injective once A*f*|grad snoise|
//    exceeds 1, and Ashima simplex peaks near |grad| ~= 4.2, so A*f >~ 0.2 already folds
//    occasionally. Mild fold-over reads as inlets and islets and is wanted; pushing A*f towards
//    1.0 would shred the coast into speckle instead.
//
// Starting point: f = 0.25 (four-field wavelength), A = 0.8 field widths -> A*f = 0.2, inside
// both bounds with room to tune. If tuning wants more range than the margin allows, add a
// second, higher-frequency octave (around f = 0.6, A = 0.2) rather than raising this one past
// the fold-over bound.
const float kWaterWarpFrequency = 0.25;   // cycles per field: a four-field wavelength
const float kWaterWarpAmplitude = 0.8;    // peak displacement, field widths
const vec2 kWaterWarpOffset1 = vec2(-128.6, 84.2);
const vec2 kWaterWarpOffset2 = vec2(63.4, -147.1);
