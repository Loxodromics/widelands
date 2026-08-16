// Tunable constants for terrain noise. Edited and re-run with no rebuild
// (unlike the three amplitudes in src/graphic/gl/terrain_noise.h, which are
// uniforms and need one). See Claude/TERRAIN_NOISE.md.

// Rotate between octaves so the simplex lattice axes never line up.
const mat2 kOctaveRotation = mat2(0.80, 0.60, -0.60, 0.80);

// Frequencies are in cycles per field. Octave 3's frequency is not a free
// parameter: two points exactly one field apart are separated most strongly
// when the wave's phase advances an odd multiple of half a cycle over that
// distance, i.e. f = n + 0.5 for integer n (0.5, 1.5, 2.5, ...). Frequencies
// at whole numbers land adjacent tiles back in phase and do almost nothing.
// Currently at n=8 (8.5), tuned up from the original 0.5 by ear over several
// steps. Octaves 1 and 2 carry no such constraint - they are free parameters
// tuned by eye for regional/mid-scale texture grain.
const float kOctave1Frequency = 1.45;
const float kOctave2Frequency = 4.05;
const float kOctave3Frequency = 8.5;

// Value field weighting (brightness), tuned by eye.
const float kValueWeight1 = 0.1;
const float kValueWeight2 = 0.1;
const float kValueWeight3 = 0.1;
const float kValueWeightSum = 0.3;

// Tint field weighting (warm/cool), tuned by eye.
const float kTintWeight1 = 2.00;
const float kTintWeight2 = 0.80;
const float kTintWeight3 = -2.00;
const float kTintWeightSum = 4.80;

// Warp frequency follows the same antiphase rule as octave 3 (see above) and
// tracks it to the same value. Currently inert: kWarpAmplitude
// (terrain_noise.h) is 0 - domain warping was disabled because it smeared
// the texture at these higher frequencies. Kept in sync in case the
// amplitude is ever raised again.
const float kWarpFrequency = 8.5;

// Chosen by capture over two ladders. Hue swing scales linearly; as mean
// |d(R-B)| in 8-bit codes, land / water: 1.5 -> 1.9/3.8, 3.0 -> 3.7/7.6,
// 5.0 -> 6.2/12.7, 8.0 -> 9.8/20.2. Below about 1 code is invisible, so 1.5
// sat near the quantization floor on land. Water takes roughly twice the land
// swing because it is heavily blue-weighted, which is what sets the ceiling.
// Clipping is not a constraint anywhere in that range (+0.24 points at worst).
const vec3 kWarmTint = vec3(1.06, 1.00, 0.92);
