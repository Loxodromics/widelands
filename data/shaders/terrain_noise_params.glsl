// Tunable constants for terrain noise. Edited and re-run with no rebuild
// (unlike the three amplitudes in src/graphic/gl/terrain_noise.h, which are
// uniforms and need one). See Claude/TERRAIN_NOISE.md.

// Rotate between octaves so the simplex lattice axes never line up.
const mat2 kOctaveRotation = mat2(0.80, 0.60, -0.60, 0.80);

// Frequencies are in cycles per field. Octave 3 carries most of the amplitude
// deliberately, and its frequency is not a free parameter: the repetition it
// has to break has a period of exactly one field, and a wave separates two
// points one period apart most strongly at an odd multiple of 0.5 cycles per
// field (antiphase). Multiples of 1.0 cycle per field land adjacent tiles
// back in phase and do almost nothing. Antiphase points so far: 0.5
// (original) -> 1.5 -> 2.5 -> 3.5 -> 4.5 -> 5.5 -> 6.5 (2026-08-16, current).
// At 6.5 the wavelength (0.154 fields, ~2.5 screen px at max zoom-out) is
// well past the ~4 px aliasing floor from §9 - unverified beyond "doesn't
// look obviously wrong in a screenshot crop"; no FFT/autocorrelation check
// has been done at this or the previous two points. Octaves 1 and 2 carry no
// such constraint - they are free parameters tuned by eye for regional/
// mid-scale texture grain.
const float kOctave1Frequency = 1.45;
const float kOctave2Frequency = 4.05;
const float kOctave3Frequency = 8.5;

// value: 1.00 / 0.50 / 1.20, normalised by 2.70   (unchanged from Phase 1)
const float kValueWeight1 = 1.0;
const float kValueWeight2 = 0.5;
const float kValueWeight3 = 1.2;
const float kValueWeightSum = 2.7;

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

// Warp frequency follows the same antiphase rule as octave 3 (see above) and
// tracks it to the same value, 6.5 as of 2026-08-16. Currently inert:
// kWarpAmplitude (terrain_noise.h) is 0 as of the same date - domain warping
// was disabled because it smeared the texture at these higher frequencies.
// Kept in sync in case the amplitude is ever raised again. See the aliasing
// note above octave 3 if it is.
const float kWarpFrequency = 8.5;

// Chosen by capture over two ladders. Hue swing scales linearly; as mean
// |d(R-B)| in 8-bit codes, land / water: 1.5 -> 1.9/3.8, 3.0 -> 3.7/7.6,
// 5.0 -> 6.2/12.7, 8.0 -> 9.8/20.2. Below about 1 code is invisible, so 1.5
// sat near the quantization floor on land. Water takes roughly twice the land
// swing because it is heavily blue-weighted, which is what sets the ceiling.
// Clipping is not a constraint anywhere in that range (+0.24 points at worst).
const vec3 kWarmTint = vec3(1.06, 1.00, 0.92);
