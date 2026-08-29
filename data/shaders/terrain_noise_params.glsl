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
// wave wander     kWanderFrequency        kWanderEvolve      kWanderOffset
// wave detail     kDetailFrequency        kDetailEvolve      kDetailOffset
// foam breakup    kFoamFrequency          tangent drift      kFoamOffset
//
// The two wave fields (WP-8a) take time as a third noise axis (snoise3(), simplex_noise_3d.glsl)
// rather than as a drift added to the sample position, so they evolve in place instead of
// translating, and their "velocity" column is the rate that third coordinate advances. The foam
// breakup (WP-10) is also 3D snoise3, but it does translate: its sample position is advected along
// the shore-frame tangent by a bounded, cross-faded drift (kFoamDriftSpeed / kFoamDriftPeriod),
// which is how the along-shore motion is expressed given there is no along-shore coordinate
// (§4.6), and its third axis carries the marching-train arc slot (a pure function of the arc
// centre, so wrap-safe). The wave *trains* themselves carry no offset -- they are sines, not
// noise, and share the wander field above rather than sampling their own. Cloud shadow is the
// remaining scrolling field; scrolling_snoise() (noise_fields.glsl, moved out of
// terrain_variation.glsl at WP-6) is its shared helper. A new field goes in this table with its
// own row when it lands.

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
//  - kGridMarginCells (shore_distance_field.h) is 28, of which 24 cells cover the
//    kMaxShoreDistance clamp and 2 more the chamfer metric's diagonal overestimate -- committed
//    before this warp existed, not free for a second purpose (found the hard way at WP-5, see
//    shore_distance_field.h's own comment). That leaves 2 cells (1 field width) of slack for this
//    warp, so kWaterWarpAmplitude must stay under 1.0 field width or the field at the frame edge
//    becomes view-dependent again and the panning gate starts failing. Raise kGridMarginCells (a
//    one-line change, +7% cells at 1080p/zoom 1 per 2-cell rise, 29 ns/cell) rather than silently
//    exceeding this.
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

// The water wash (water.fp; Claude/WATER.md WP-6/WP-7): a colour composited over the seabed the
// terrain pass draws for water triangles, with its edge coming from water_shore_warp()'s field
// rather than the field lattice. WP-7 keys both colour and opacity off |shore| itself (via
// water.fp's depth_t = clamp(shore / u_max_distance, 0.0, 1.0), reusing the uniform already bound
// to ShoreDistanceField::kMaxShoreDistance rather than adding a second width constant): a river
// narrower than that span never reaches the deep stop, with no special-case code, because it is
// never far from a shore.
//
// The three colour stops are the Appendix's measured reference ramp verbatim -- kWaterColorMid is
// what WP-6 shipped as the single flat kWaterColor, now the ramp's midpoint rather than its only
// value. The Appendix ramp is a hue swing toward cyan, not a brightness ramp (green rises 73%
// across it against 43% for red/blue), which is why these are three independent stops rather than
// a single-axis brightness mix.
//
// kWaterOpacityDeep is unchanged from WP-6's kWaterOpacity: high enough that deep water does not
// show the seabed through it (the Appendix's complaint about our own water texture was the
// opposite failure -- a wash so weak the floor read as visible everywhere), while still leaving a
// tenth of the seabed subtly readable per §4.4 ("depth controls how much of it survives").
// kWaterOpacityShallow is the one number here with no reference measurement behind it -- lower, so
// the seabed WP-6 draws genuinely shows through right at the shoreline, tuned by eye against a
// capture rather than transcribed.
//
// kWaterEdgeWidth is a separate mechanism from the depth ramp above: it is in field widths (the
// same unit |shore| is stored in) -- one whole cell either side of the coastline (kCellSize = 32
// against a 64 px field, so one cell is 0.5 field widths) -- and only antialiases the coastline
// edge itself, at a scale (0.5 field widths) far narrower than the depth ramp's (up to
// kMaxShoreDistance = 12 field widths). water.fp's max(kWaterEdgeWidth, 0.5 * fwidth(shore)) floor
// follows what kDitherMinWidth does for dither.fp, but unlike there it does not currently bind at
// any zoom the game offers: at the game's maximum zoom-out the fwidth() term reaches only about
// 0.08 field widths (including the warp's gradient amplification) against kWaterEdgeWidth's 0.5.
// Kept as a guard: WP-9's foam band turned out to be *additional* to this transition rather than
// a narrowing of it, so the floor still does not bind -- it is there for any future change that
// does narrow the transition far enough for it to matter.
const vec3 kWaterColorShallow = vec3(100.0, 190.0, 215.0) / 255.0;
const vec3 kWaterColorMid = vec3(80.0, 135.0, 175.0) / 255.0;
const vec3 kWaterColorDeep = vec3(70.0, 110.0, 150.0) / 255.0;
const float kWaterOpacityShallow = 0.5;  // STARTING POINT -- tuned against a capture
const float kWaterOpacityDeep = 0.9;     // unchanged from WP-6's kWaterOpacity
const float kWaterEdgeWidth = 0.5;  // one whole cell either side of the coastline, in field widths

// Wet sand (water.fp main(), Claude/WATER.md §4.4, WP-12): the negative side of the signed shore
// field darkens and warm-tints a narrow strip of land at the waterline, so a beach reads as damp
// where the water meets it. Shader-only -- no new uniform, sampler or noise field (hence no row in
// the offset budget above).
//
// kWetSandTint is a MIX TARGET, not the colour of wet sand. The water pass is kBlendAlpha, so it
// cannot read the land colour back and cannot apply a true saturation boost; all it can do is mix
// the destination toward a constant. The target is near-black, so the operator is essentially a
// darkening, and warm (hue ~26 degrees) so the damp margin shifts toward brown rather than toward
// grey.
//
// It does NOT deliver the "and saturate" half of WP-12's brief, and the first cut of this comment
// claimed that it did. The arithmetic behind that claim -- a sand swatch (0.75, 0.65, 0.48) mixed
// 0.32 toward this target takes HSV S 0.360 -> 0.381 -- is correct but does not generalise: HSV
// saturation is not linear under an RGB mix, so the sign of the change depends on how close the
// source hue is to the target's, and it was never checked against a render. Measured over the
// affected land pixels of the water_coast golden (WATER.md WP-12's review pass): mean S goes
// 0.371 -> 0.348 and V 0.457 -> 0.397. By hue, warm/sandy sources (20-60 deg, matching the tint)
// are a wash at -0.002 with S rising on 53% of them, while green coasts (90-160 deg, most of that
// scene) lose 0.027. The DARKENING is what carries the effect. Treat the hue shift as a side
// effect to watch, not as a feature, and if a chroma rise is ever wanted it needs a different
// mechanism than a mix toward a constant.
//
// kWetSandStrength is the PEAK ALPHA, reached exactly on the waterline (shore = 0) and falling to
// zero at both ends of the strip. It is the first lever if the contact line reads as a hard dark
// rim.
//
// The effect is deliberately TERRAIN-AGNOSTIC: a meadow or rock coast darkens at the waterline
// too, which is right -- wet ground is darker whatever it is made of. Making it sand-only is not
// cheap: the water pass has no terrain identity at the fragment, and the SDF payload's nearest-land
// terrain (shore_distance_field.h, WP-11) is the wrong quantity on the land side of the field.
//
// kWetSandWidth is the inland reach in field widths. 0.5 is one SDF cell (32 px at zoom 1) and
// equals kWaterEdgeWidth, so the whole effect stays inside the -w < shore < +w strip that is
// already rasterised and water.fp's early-out (which gates on max(kWetSandWidth, w)) is unchanged.
// Raising this past kWaterEdgeWidth widens that gate rather than clipping the band -- which is a
// property of the gate, so keep it gating on the max if this constant is ever tuned.
const vec3 kWetSandTint = vec3(0.10, 0.06, 0.03);
const float kWetSandStrength = 0.35;  // STARTING POINT -- tuned against a capture
const float kWetSandWidth = 0.5;      // inland reach, field widths: one SDF cell, 32 px at zoom 1

// Wave surface motion (water_wave_field(), water.fp; Claude/WATER.md §4.6, WP-8a): three
// travelling wave trains, each a sharpened sine whose crest lines run perpendicular to its own
// direction, plus one fine detail layer. WP-8 built this as two scrolled simplex layers instead;
// that read as smooth blobs sliding, because a translating noise field has no crest structure and
// nothing in it ever forms or breaks. A sine train has both for free, and sharpening it (below)
// is what makes the surface legible at a swing an observer barely notices when it is spread over
// smooth noise.
//
// Directions are unit vectors in var_texture_position's frame (map pixels / 64, y negated), which
// mirrors them vertically against map space -- immaterial, since the directions are arbitrary and
// a reflection preserves the angles between them.
const vec2 kWaveDirA = vec2(0.951, 0.309);    // 18 degrees
const vec2 kWaveDirB = vec2(0.469, 0.883);    // 62 degrees
const vec2 kWaveDirC = vec2(0.906, -0.423);   // -25 degrees

// Wavenumbers (radians per field width) and angular frequencies (radians per second) for
// wavelengths of 1.8 / 1.05 / 0.6 field widths at periods of 2.50 / 1.91 / 1.44 seconds.
//
// The periods are not free: deep-water waves disperse as T = c * sqrt(lambda), and these follow
// that law at c = 1.86, so the long train travels fastest (0.72 / 0.55 / 0.42 field widths per
// second, i.e. 46 / 35 / 27 map pixels per second at zoom 1.0). That is what makes the
// interference between the trains form and dissolve instead of repeating: three trains at a
// common speed would keep a fixed relative phase. Keep the relation if these are retuned.
const float kWaveNumberA = 3.491;   // 2*pi / 1.8
const float kWaveNumberB = 5.984;   // 2*pi / 1.05
const float kWaveNumberC = 10.472;  // 2*pi / 0.6
const float kWaveOmegaA = 2.513;    // 2*pi / 2.50
const float kWaveOmegaB = 3.290;    // 2*pi / 1.91
const float kWaveOmegaC = 4.363;    // 2*pi / 1.44

// One train dominates and the other two perturb it. Equal weights do not work: three sharpened
// sines of comparable weight average into blotches with no crest lines left, because each one's
// troughs fill the others' crests (tried, and it is what the first tuning pass looked like).
const float kWaveWeightA = 1.0;
const float kWaveWeightB = 0.22;
const float kWaveWeightC = 0.14;
const float kWaveWeightSum = 1.36;
const float kWaveLambdaC = 0.6;  // the chop train's wavelength, in field widths; drives the fade

// Crest shaping: crest = pow(0.5 + 0.5*sin(phase), sharpness), which narrows the crest and widens
// the trough the way real water does. The mean of that expression is not 0.5, so water.fp
// subtracts it (wave_crest_mean()) to keep the sharpening from shifting the water's base tone.
// The sharpness itself fades toward 1.0 when zoomed out -- see kCrestFade*Px below.
const float kCrestSharpness = 2.2;

// One shared noise field displaces every train along its own direction, so crest lines bend
// rather than running dead straight, and -- because the field's third axis is time, not a drift
// velocity -- the bends themselves appear and dissolve in place instead of sliding past. Sampled
// through snoise3() (simplex_noise_3d.glsl), one evaluation for all three trains.
const float kWaveWanderAmp = 0.22;    // peak displacement, field widths
const float kWanderFrequency = 0.35;  // cycles per field
const float kWanderEvolve = 0.15;     // third-axis units per second

// Shore phase: bends the crests near the coast, WP-8a's stand-in for refraction. Added to each
// train's phase (water.fp's wave_train()), never multiplied into its wavenumber -- see that
// function's comment for why a depth-varying wavenumber destroys the field far from the map
// origin. In radians, so a train's crests shift by at most this over its own wavenumber, in field
// widths: at 2.0 radians that is a third of a wavelength for every train alike.
const float kShorePhase = 2.0;

// Fine detail between the crests: a second snoise3() evaluation, also evolving in place.
const float kDetailFrequency = 3.5;  // cycles per field
const float kDetailWeight = 0.15;
const float kDetailEvolve = 0.6;  // third-axis units per second

// Whitecaps (water.fp main(), Claude/WATER.md §6 / WP-9a). WP-8a built this as a quiet near-white
// "glint" thresholded on the coincidence of the three wave trains: a deliberate, low-amplitude
// partial take on §6's whitecaps, added additively so it read as sparkle. WP-9a promotes the same
// term -- retuned, and re-composited as foam rather than as brightening -- so the caps sit *on*
// the water instead of lightening it. It is the same crest field, not a parallel one: two stacked
// highlights over one crest sum would double up.
//
// - Lower threshold than WP-8a's 0.68, so more crests pass and the result reads as streaks rather
//   than as isolated sparkle.
// - Composited by mixing toward kFoamColor and contributing to opacity, the way the shore band is
//   (main()). kFoamColor is the measured reference foam tone -- no reason for a second white -- so
//   kGlintColor is gone.
// - Both WP-8a gates kept: the depth_t gate keeps whitecaps off the waterline where the shore
//   band owns the look, and crest_fade drops them when crests get too small on screen to hold
//   detail.
// - Combined with the shore band by max() before a single mix and a single opacity term, so the
//   two never stack two mixes toward the same colour, and §4.8's cloud-shadow cancellation stays
//   intact (foam folds into color before the * var_cloud_shadow, alpha stays cloud-independent).
//
// kWhitecapStart and kWhitecapStrength have no model behind them: crest depends on the three wave
// trains and snoise3(), which is not ported, so both are A/B'd on captures and the achieved
// near-white water fraction is measured (WATER.md WP-9a), not asserted.
const float kWhitecapStart = 0.5;     // crest sum, in [0, 1], at which whitecaps begin
const float kWhitecapStrength = 0.7;   // foam coverage the strongest crest coincidences reach
const float kWhitecapDepthMin = 0.05;  // depth_t gate: no whitecaps right at the waterline,
const float kWhitecapDepthMax = 0.15;  // the shore band owns that zone
// How far the detail field can pull the crest sum down before it is thresholded, so the whitecaps
// scatter instead of repeating on the trains' own beat lattice (water_wave_field()).
const float kCrestMaskFloor = 0.55;

// Zoom fades, in screen pixels per wavelength (water.fp derives pixels-per-field from
// fwidth(var_texture_position.x), the same idiom kDitherGrainFadeMin/Max uses). Sharpened crests
// concentrate their energy into roughly a quarter wavelength, so the chop train reaches the
// aliasing floor before its wavelength does: at kMaxZoom its 0.85 fields are 13.6 screen pixels.
// Below kCrestFadeMinPx the sharpening is gone (sharpness 1.0, a plain sine) and below
// kDetailFadeMinPx the detail layer is gone entirely.
const float kCrestFadeMinPx = 10.0;
const float kCrestFadeMaxPx = 24.0;
const float kDetailFadeMinPx = 5.0;
const float kDetailFadeMaxPx = 12.0;

// Amplitude varies by *depth*, not speed by depth: a spatially varying speed shears the field
// (a time term multiplied by a spatially varying factor drifts the effective phase apart by
// hundreds of cycles between depth levels within a few minutes, tearing the surface along the
// depth contours). Modulating amplitude instead calms the water near the shoreline, where WP-9's
// foam band takes over. STARTING POINT.
const float kWaveAmplitudeShallow = 0.75;
const float kWaveAmplitudeDeep = 1.0;

// Colour swing, applied along the wash's own shallow/deep hue axis (crests cyan-ward and lighter,
// troughs blue-ward and darker -- the axis the Appendix's reference ramp measurably moves along,
// and physically the right one: a crest is less water above the seabed). Written out as a literal
// rather than derived from kWaterColorShallow/Deep so it does not depend on declaration order.
//
// Quote the *measured* effect, not the notional peak: WP-8's comment gave only its wave = +/-1
// peak, which its own smooth-noise blend reached so rarely that the visible result was four times
// smaller (mean 2, p95 5, max 11 8-bit codes -- barely above kWarmTint's "below about 1 code is
// invisible"). Measured the same way (differencing against a capture with this constant zeroed,
// over the water fragments of water_coast_zoom4): PLACEHOLDER -- filled in after tuning.
const vec3 kWaveColorSwing = vec3(0.18, 0.45, 0.37);

// Wraps u_time a second time, independent of kCloudTimeWrapPeriod (terrain_noise.h, 100000s), to
// keep the time-derived terms in a range where float32 still resolves them: the largest is the
// chop train's phase, omega_C * wave_time, which reaches 3.7e4 radians at this period (a float32
// step of 4.4e-3 radians there, invisible inside a sin()), and the two snoise3() third-axis
// coordinates reach 1.5e3 and 6e3. At kCloudTimeWrapPeriod itself they would be ten times that,
// where the noise fields start quantising visibly against their per-screen-pixel step. 10000s
// divides kCloudTimeWrapPeriod exactly, so no second, irregular discontinuity is added on top of
// the outer wrap; the cost is one pattern discontinuity roughly every 2.8 gametime hours.
const float kWaveTimeWrapPeriod = 10000.0;

// Arbitrary; only need to land far from p=0 and from every other offset in the budget table above
// (same technique as kTintOffset).
const vec2 kWanderOffset = vec2(-183.5, 29.7);
const vec2 kDetailOffset = vec2(112.9, -203.4);

// Foam band (water.fp, WP-9/WP-9a/WP-9b/WP-9c; animated at WP-10; Claude/WATER.md §4.6): stylised
// shore-parallel foam lines in the shallowest water zone, thinning and fading offshore, as in
// referenceImages/SebastianLague00.jpg. All distances are in field widths -- the unit |shore| is
// stored in.
//
// WP-9 drew one near-opaque plateau; WP-9a dissolved it against a coverage field with a one-sided
// threshold remap so it holes and streaks; WP-9b/WP-9c segmented it into three tight arcs. WP-10
// replaces the three fixed arcs with a MARCHING TRAIN and gives the breakup an along-shore drift.
//
// The dissolve (foam_arc(), water.fp): cov falls smoothly from 1 at the arc centre to 0 at its
// half-width with no plateau, and foam appears wherever cov exceeds a noise-driven threshold
//   thresh = kFoamDissolve + (1 + overshoot - kFoamDissolve) * u,  u = clamp(0.5 + 0.5*noise, 0, 1)
// where the coverage decides how much foam and the noise decides where. thresh >= kFoamDissolve by
// construction, so smoothstep(thresh - kFoamDissolve, ..., cov) == 0 exactly wherever cov == 0,
// i.e. outside the arc's half-width: the foam stays self-limiting, which keeps kFoamReach a tight
// bound and the land side clear.
//
// THE MARCHING TRAIN (WP-10). Arc centres are
//   base   = kFoamTrainBase - kFoamTrainSpacing * fract(u_time / kFoamMarchPeriod)
//   centre = base + kFoamTrainSpacing * k          (k integer)
// so the whole set steps one spacing shoreward every kFoamMarchPeriod and then wraps. Every arc
// property is derived from its CURRENT CENTRE, never from k:
//   - profile: c_t = clamp(centre / kFoamProfileSpan, 0, 1), then mix(near, far, c_t) for half
//     width, overshoot and strength;
//   - life: smoothstep(kFoamDeathStart, kFoamDeathEnd, centre) fades the arc out as it crosses the
//     waterline (swash sinking into the sand); (1 - smoothstep(kFoamBirthStart, kFoamBirthEnd,
//     centre)) fades it in as it is born offshore, keeping it clear of the reach gate's hard edge.
// Because nothing depends on k, when fract wraps 1 -> 0 arc k takes over exactly the centre, width,
// overshoot, strength and life arc k-1 just had: the render is continuous through the wrap with no
// cross-fade and no pop. This is THE invariant to preserve -- any per-arc quantity that is a
// function of k rather than centre reintroduces a visible jump every kFoamMarchPeriod.
//
// The near/far profile endpoints are WP-9c's own three arcs (0.10/0.11/0.35/1.00, 0.40/0.08/0.18/
// 0.65, 0.65/0.07/0.10/0.45) fitted by a line through centre 0.10 and 0.65 and extended to centre
// 0 -- except kFoamHalfWidthNear, which the separation check below pulls in under the fit. At
// march = 0 the train sits at (centre, strength, half width) (0.10, 1.00, 0.095),
// (0.39, 0.71, 0.082), (0.68, 0.42, 0.070) against WP-9c's (0.10, 1.00, 0.11), (0.40, 0.65, 0.08),
// (0.65, 0.45, 0.07) -- the committed look to within a few percent, which makes the --at 0 golden
// diff small and interpretable.
//
// SEPARATION is the binding constraint on the spacing and on kFoamHalfWidthNear, and it has to be
// checked over the WHOLE MARCH CYCLE rather than at march = 0. This is WP-9b's lesson restated for
// a moving train: consecutive arcs must be separated by clear water or they read as one wider,
// more diffuse foam zone instead of as lines. A uniform spacing with a profile that widens toward
// the shore is TIGHTER at its worst phase than the static set it replaces, because two arcs can
// straddle the near end where both are at their widest -- averaging WP-9c's 0.30 / 0.25 gaps hides
// that entirely. Measured as the clear water between neighbouring arcs (cov == 0 on both sides),
// minimised over the cycle: this set gives 6.4 px at zoom 1 against WP-9c's own 7.0 / 6.4, and
// never shows fewer than three arcs. The first WP-10 constants (spacing 0.275, kFoamHalfWidthNear
// 0.117) gave 3.5 px -- back in the range that failed at WP-9b, confirmed on a capture as a
// diffuse halo rather than separate lines. If a centre, a spacing or a half width moves, re-run
// that worst-phase envelope check; per-arc statistics cannot catch this.
//
// NEAREST-ARC EVALUATION. The train is periodic in shore and the arcs never overlap, so a fragment
// is inside at most one arc's half width and
//   k = floor((shore - base) / kFoamTrainSpacing + 0.5)
// picks it: one foam_arc() call, exactly equal to a max() over all arcs because foam_arc() is
// identically zero outside its own half width. The non-overlap invariant it rests on is
//   max(half_width, kFoamMinWidthPixels * fwidth(shore)) < kFoamTrainSpacing / 2  ( = 0.145 )
// The widest profile half width is 0.100 and the pixel floor reaches ~0.12 at maximum zoom-out, so
// it holds with room. foam_arc() hard-caps hw at kFoamArcHalfWidthCap * kFoamTrainSpacing
// ( = 0.1334 ) so a later constant change or a larger kMaxZoom cannot violate it; a violation
// would show as a hard seam midway between arcs, not a soft artifact. The cap sits above the pixel
// floor, so the floor still does its own job at maximum zoom-out rather than being clipped away.
//
// ALONG-SHORE DRIFT (foam_noise(), water.fp). §4.6: there is no along-shore coordinate to advance,
// so the breakup SAMPLE POSITION is advected along the shore-frame tangent by D = kFoamDriftSpeed *
// kFoamDriftPeriod field widths on a sawtooth that resets every kFoamDriftPeriod. Two layers half a
// period out of phase are cross-faded (each weight zero at its own reset) so the wrap is free; a
// blend-RMS normalisation with the measured layer correlation kFoamDriftLayerRho keeps the variance
// constant across the cross-fade, or the breakup's raggedness would pulse at kFoamDriftPeriod.
//
// That correlation must be measured at the separation the two layers actually have, which is NOT
// D. The layers sit at D*f0 and D*f1 with f1 = fract(f0 + 0.5), so |f0 - f1| is identically 0.5 and
// the separation is ALWAYS EXACTLY D/2, at every point of the cycle -- which is also why a single
// constant rho is the right model. Measuring at D instead under-estimates rho and makes the
// correction overshoot: at rho = 0.64 (the value for separation D) the mid-cross-fade contrast
// comes out 7.7 % HIGH, where leaving the normalisation out entirely is only 2.5 % low, i.e. the
// fix would have been worse than the defect. Measured with the numpy Ashima port at separation
// D/2 = 0.125: rho = 0.90, which holds the contrast flat to within a fraction of a percent.
//
// The tangent flips sign across a channel's medial axis, so opposite banks advect in opposite
// directions -- an accepted consequence of unidirectional advection; kFoamDriftConfMin/Max fade the
// drift out where the frame is ill-defined (|grad shore| collapses on that ridge). Keep D well
// under one noise wavelength (1 / kFoamFrequency = 1.11): D = 0.25 keeps the two layers correlated
// enough that the blend does not read as a double exposure. Raising the drift speed means raising D
// or shortening the period; both move the layer separation D/2, so kFoamDriftLayerRho has to be
// re-measured if either changes, and a shorter period pulses the cross-fade more often.
//
// PER-ARC DECORRELATION moved to the third noise axis. WP-9c's kFoamStagger* displaced each arc's
// tap along the tangent so neighbours would not break in the same places; tied to k it jumps at the
// wrap, tied to centre it slides the dashes along the shore and fights the drift. Instead foam_noise
// puts the arc slot (centre / kFoamTrainSpacing) on snoise3's third axis, separated by
// kFoamArcAxisStep -- wrap-safe, and as a side effect each arc's dash pattern re-forms slowly as it
// travels in, which is right for a wave approaching the beach.
//
// Tuned for zoom 1 and nothing else. At maximum zoom-out the pixel floor is wider than the arcs so
// they merge into one band; accepted, not fixed. If a capture reads faint the levers are
// kFoamStrengthNear/Far, kFoamOpacity, or a lower kFoamOvershootNear/Far, in that order.
const float kFoamFrequency = 0.9;   // cycles per field: ~1.1-field wavelength, which sets the
                                    // dash length -- runs average 48 px at zoom 1 (WP-9c)
const float kFoamMinWidthPixels = 1.5;
// 1 / p95(|snoise3|) for one tap at kFoamFrequency, measured with the numpy Ashima port: RMS 0.373,
// p95 0.696, max ~1.0. Coupled to kFoamFrequency and to the 3D field -- re-measure with the port if
// either moves. It rose from WP-9c's 1.27 because snoise3 (0.6/42 Ashima variant) has a smaller p95
// than the 2D snoise WP-9c used.
const float kFoamNoiseScale = 1.44;
const float kFoamDissolve = 0.20;   // threshold softness, in coverage units; raises the partial fraction
const float kFoamOpacity = 0.95;  // near 1: the band hides the seabed and reads white, not sand
const float kFoamGradStep = 0.5;  // shore-frame finite-difference step: one grid cell
const float kFoamGradEpsilon = 0.05;  // plateau guard, on the frame's gradient magnitude

// The marching train (WP-10). "Near" is the profile at centre 0 (the waterline), "Far" at
// centre >= kFoamProfileSpan; both are WP-9c's fitted line (see the fit note above).
const float kFoamTrainSpacing = 0.29;    // pinned by the worst-phase separation check above, not
                                         // by averaging WP-9c's 0.30 / 0.25 arc gaps
const float kFoamTrainBase = 0.10;       // innermost centre at march = 0; reproduces WP-9c
const float kFoamMarchPeriod = 5.0;      // s per spacing; 3.7 px/s shoreward at zoom 1. Divides
                                         // kWaveTimeWrapPeriod (10000 / 5 = 2000)
const float kFoamProfileSpan = 0.65;     // centre at which the profile reaches its "far" end
const float kFoamHalfWidthNear = 0.100;  // below the fitted 0.115: the separation check binds here
const float kFoamHalfWidthFar = 0.070;
const float kFoamOvershootNear = 0.395;
const float kFoamOvershootFar = 0.100;
const float kFoamStrengthNear = 1.10;
const float kFoamStrengthFar = 0.45;
const float kFoamDeathStart = -0.12;     // life envelope: fade out across the waterline ...
const float kFoamDeathEnd = 0.04;
const float kFoamBirthStart = 0.65;      // ... and fade in offshore, clear of the reach gate. Set
const float kFoamBirthEnd = 0.85;        // so the outermost arc stays visible at the wider
                                         // spacing -- at 0.62 / 0.80 the count drops to two arcs
const float kFoamArcHalfWidthCap = 0.46; // of kFoamTrainSpacing; guards the non-overlap invariant

// Along-shore drift (WP-10, foam_noise()).
const float kFoamDriftSpeed = 0.08;      // field widths/s along shore: 5.1 px/s at zoom 1
const float kFoamDriftPeriod = 3.125;    // s; D = kFoamDriftSpeed * kFoamDriftPeriod = 0.25 fields.
                                         // Divides kWaveTimeWrapPeriod (10000 / 3.125 = 3200)
const float kFoamDriftLayerRho = 0.90;   // measured (numpy port) at the layers' true separation
                                         // D/2 = 0.125, not at D -- see the drift note above;
                                         // blend-RMS normalisation, exactly 1.0 at the resets
const float kFoamArcAxisStep = 0.45;     // third-axis separation per arc slot (WP-9c's measure)
const float kFoamDriftConfMin = 0.35;    // on |grad shore|: fade the drift out below this, full
const float kFoamDriftConfMax = 0.70;    // above -- guards the medial-axis tangent flip

// The inner gate. The visible outer edge of the foam is unchanged from WP-9c at ~0.72 field widths
// (46 px at zoom 1); the extra span to 0.97 is the birth fade only (kFoamBirthEnd 0.85 plus the
// max(half width, pixel-floor ~0.12), rounded up), so an arc entering through the gate is not
// clipped into the hard contour edge the dissolve exists to avoid. Moving kFoamBirthEnd or the
// pixel floor means redoing this arithmetic.
const float kFoamReach = 0.97;
// Measured from referenceImages/AoE2_1.png (the bright foam at the land contact), 9x9 patch
// averages as the Appendix measures: 24 patches, mean (189, 217, 228), median (184, 214, 226).
// The plan's fallback (230, 240, 245) is the same cool white but brighter than the reference's
// foam actually reads -- a white that bright would bloom against the wash.
const vec3 kFoamColor = vec3(189.0, 217.0, 228.0) / 255.0;

// The x/y offset of the breakup field (WP-10 samples it through snoise3 with the arc slot on z).
// Arbitrary; checked against the budget table at the top (per its own instruction) rather than
// against a neighbouring constant: lands far from every offset listed there, in the one quadrant
// none of them reach (largest x, largest positive y).
const vec2 kFoamOffset = vec2(226.4, 174.9);
