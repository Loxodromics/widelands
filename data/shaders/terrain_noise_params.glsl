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
// foam breakup    kFoamFrequency          static             kFoamOffset
// foam fine       kFoamFineFrequency      static             kFoamFineOffset
//
// The two wave fields (WP-8a) are the only 3D ones: they take time as a third noise axis
// (snoise3(), simplex_noise_3d.glsl) rather than as a drift added to the sample position, so they
// evolve in place instead of translating, and their "velocity" column is the rate that third
// coordinate advances. The wave *trains* themselves carry no offset -- they are sines, not noise,
// and share the wander field above rather than sampling their own. Cloud shadow is the remaining
// scrolling field; scrolling_snoise() (noise_fields.glsl, moved out of terrain_variation.glsl at
// WP-6) is its shared helper. A new field goes in this table with its own row when it lands.

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

// Foam band (water.fp, WP-9; reworked at WP-9a; three arcs at WP-9b; Claude/WATER.md §4.6): pale
// foam in the shallowest water zone, its breakup elongated along the coast by the shore frame. All
// distances are in field widths -- the unit |shore| is stored in.
//
// WP-9 drew one near-opaque plateau; WP-9a dissolved it against a coverage field with a one-sided
// threshold remap so it holes and streaks at the core and fades at its margins. WP-9b segments the
// result into three arcs running parallel to the coast, getting thinner and fainter offshore, as
// in referenceImages/SebastianLague00.jpg -- a bright solid line at the waterline, then two
// progressively thinner arcs stepping out to sea. This is the WP-9a band body (foam_arc(),
// water.fp) evaluated three times against the same single foam_noise() call; the expensive taps
// (four shore_at(), two snoise_grad(), four breakup taps) do not repeat.
//
// The dissolve, per arc: cov falls smoothly from 1 at the arc centre to 0 at its half-width with
// no plateau, and foam appears wherever cov exceeds a noise-driven threshold
//   thresh = kFoamDissolve + (1 + overshoot - kFoamDissolve) * u,  u = clamp(0.5 + 0.5*blend, 0, 1)
// where the coverage decides how much foam and the noise decides where. The clamp matters -- the
// coarse/fine blend's max |value| exceeds 1. thresh >= kFoamDissolve by construction, so
// smoothstep(thresh - kFoamDissolve, ..., cov) == 0 exactly wherever cov == 0, i.e. outside each
// arc's own half-width: the foam stays self-limiting per arc (modelled max foam outside all three
// arcs 0.0000), which is what keeps kFoamReach a tight bound and the land side clear.
//
// Two design questions were settled by modelling on a straight coast (numpy Ashima port), not
// guessed (WATER.md WP-9b):
//  - The arcs need no per-arc noise decorrelation. Each one samples the breakup field at its own
//    distance offshore, so the measured along-shore correlation between arc foam profiles is
//    already near zero (A-B -0.12, B-C -0.04, A-C -0.02, this machine). Per-arc coarse/fine blends
//    and per-arc threshold bias were both planned and both dropped as unnecessary.
//  - Continuity is controlled per arc by 'overshoot', not by the dissolve. A lower overshoot on
//    the outer arcs gives the clean stylised lines the reference has, while arc A keeps WP-9a's
//    ragged contact zone. Modelled core (d < 0.25) empty/partial/solid at kFoamDissolve = 0.20:
//    A 16 / 31 / 53 %, B 6 / 29 / 65 %, C 2 / 25 / 73 % (this machine). Higher overshoot = more
//    holes: for arc A's geometry, 0.00 -> 0/19/81, 0.18 -> 6/29/65, 0.35 -> 16/31/53,
//    0.70 -> 38/27/36.
//
// The three centres step by roughly 0.6 and 0.4 field widths, and half widths and strengths fall
// by ~0.6x per arc -- a geometric law, so the set reads as deliberate rather than as three
// independent guesses. If a capture reads faint, the levers are kFoamArcStrength* / kFoamOpacity
// or a lower kFoamArcOvershoot*, in that order, tuned on the capture.
//
// kFoamArcCentreA is a centre, not a falloff-from-zero: kWaterEdgeWidth = 0.5 field widths is a
// genuinely soft transition (coverage is ~0.5 at shore = 0), so foam peaking at the waterline
// would be half transparent over sand and read as pale beach rather than white foam. 0.35 puts
// the peak where coverage is ~0.85, still visually at the contact.
//
// WP-9a's second higher-frequency octave (kFoamFineFrequency) stays: on a straight coast raising
// the coarse octave's amplitude only deepens the edge indentations while the ribbon stays
// continuous; the fine octave is what detaches patches and opens holes. It is a single tap,
// deliberately not elongated -- fine-scale foam texture is not strongly directional, and one tap
// keeps the total at four while carrying more amplitude for the same weight (the three-tap tangent
// box costs half the RMS: raw snoise RMS 0.47, three-tap 0.25).
//
// The pixel floor under each half-width is the kDitherMinWidth idiom: at maximum zoom-out
// fwidth(shore) reaches ~0.08 field widths (WP-9, measured at _zoom4), so
// kFoamMinWidthPixels * fwidth(shore) ~= 0.12 -- it does not bind for any arc, but arc C's 0.18 is
// the narrowest the series has had, so the margin is thinner than WP-9a's 0.5 gave.
//
// The arcs are static. Advancing their centres shoreward over time is WP-10 (Animate the foam),
// which must advect the sample position along the frame tangent by a bounded offset rather than
// advance an unbounded along-shore coordinate (§4.6); it is deliberately not pre-empted here.
const float kFoamFrequency = 0.9;       // coarse octave, cycles per field: ~1.1-field wavelength,
                                        // sets the band's large-scale shape
const float kFoamStretch = 0.55;        // field widths along the shore tangent; the three coarse
                                        // taps span ~one coarse wavelength, elongating the breakup
const float kFoamFineFrequency = 2.6;   // fine octave, ~0.38-field: the octave that breaks the ribbon
const float kFoamCoarseWeight = 0.62;   // coarse : fine, sum to 1 (so no separate weight-sum constant)
const float kFoamFineWeight = 0.38;
const float kFoamMinWidthPixels = 1.5;
// 1 / p95(|blend|), measured with the numpy Ashima port: raw weighted coarse+fine blend RMS
// 0.236, p95 0.449, max 0.847. Coupled to the two frequencies, the stretch and the two weights --
// re-measure with the port if any of them move. Unchanged at WP-9b: none of them moved.
const float kFoamNoiseScale = 2.23;
const float kFoamDissolve = 0.20;   // threshold softness, in coverage units; raises the partial fraction
const float kFoamOpacity = 0.95;  // near 1: the band hides the seabed and reads white, not sand
const float kFoamGradStep = 0.5;  // shore-frame finite-difference step: one grid cell
const float kFoamGradEpsilon = 0.05;  // plateau guard, on the frame's gradient magnitude

// The three arcs (WP-9b). Naming follows kWaveDirA/B/C (WP-8a) rather than numeric suffixes.
// A is WP-9a's band unchanged (the turbulent contact zone); B is a thinner, more continuous line;
// C is a thin, nearly unbroken outer line. Geometric law: centres +~0.6/+~0.4 field widths, half
// widths and strengths x~0.6 per step.
const float kFoamArcCentreA = 0.35;
const float kFoamArcHalfWidthA = 0.50;
const float kFoamArcOvershootA = 0.35;
const float kFoamArcStrengthA = 1.00;
const float kFoamArcCentreB = 0.95;
const float kFoamArcHalfWidthB = 0.30;
const float kFoamArcOvershootB = 0.18;
const float kFoamArcStrengthB = 0.65;
const float kFoamArcCentreC = 1.35;
const float kFoamArcHalfWidthC = 0.18;
const float kFoamArcOvershootC = 0.10;
const float kFoamArcStrengthC = 0.40;
// kFoamArcCentreC + kFoamArcHalfWidthC: the inner gate. With the one-sided remap every arc is
// exactly zero beyond its own half-width (max foam outside all arcs == 0, modelled), so this is a
// tight bound -- but only while each arc's half-width binds over the pixel floor above. Arc C's
// 0.18 is the narrowest and the first at risk if the floor ever binds; then that arc extends past
// this gate and gets a hard contour edge in open water instead of dissolving.
const float kFoamReach = 1.53;
// Measured from referenceImages/AoE2_1.png (the bright foam at the land contact), 9x9 patch
// averages as the Appendix measures: 24 patches, mean (189, 217, 228), median (184, 214, 226).
// The plan's fallback (230, 240, 245) is the same cool white but brighter than the reference's
// foam actually reads -- a white that bright would bloom against the wash.
const vec3 kFoamColor = vec3(189.0, 217.0, 228.0) / 255.0;

// Arbitrary; checked against the budget table at the top (per its own instruction) rather than
// against a neighbouring constant: lands far from every offset listed there, in the one quadrant
// none of them reach (largest x, largest positive y).
const vec2 kFoamOffset = vec2(226.4, 174.9);
// The fine octave's own offset (WP-9a), checked against the whole budget table: nearest listed
// offset is kWaterWarpOffset1 (-128.6, 84.2) at ~220 field-width units, comfortably farther than
// the spacing between several existing pairs, and the two fields differ in frequency by 10x
// besides. Lands in the largest-positive-y strip none of the others reach.
const vec2 kFoamFineOffset = vec2(-91.2, 301.5);
