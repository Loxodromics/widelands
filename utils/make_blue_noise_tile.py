#!/usr/bin/env python3
# encoding: utf-8

"""Generate a void-and-cluster blue-noise threshold tile.

The tile is the dither threshold source for the water pass (Claude/WATER.md WP-16,
Claude/WATER_WP16_NOTES.md). It is a generated maths asset, not artwork: the committed PNG is
reproducible from this script and a seed, so regenerate rather than hand-edit.

Algorithm: Ulichney, "The void-and-cluster method for dither array generation" (1993). The
filter is a wrapped Gaussian, which is what makes the result tile seamlessly -- the array is
built on a torus from the start rather than being made periodic afterwards.

Output is an 8-bit greyscale PNG whose pixel value is the dither threshold. The rank r of a
pixel (its position in the void-and-cluster ordering, 0 .. n-1 with every rank used exactly
once) is stored as floor(r * 256 / n), so for a power-of-two tile every code 0..255 is used by
exactly the same number of pixels. Sample it in the shader as texture(...).r and treat the
result as a threshold in [0, 1].

The pixel values are data, not colour, so a lossy PNG optimiser would corrupt the dither.
utils/run_pngquant.sh walks the whole of data/ when called without an argument, but it happens
to be harmless here on two counts: pngquant's output is bigger than the input, so the script's
5%-shrink gate skips the file, and an 8-bit tile already holds exactly 256 distinct values so
quantising to 256 colours is lossless anyway. Neither protection survives --bits 16. The tools
in utils/optimize_pngs.py (advdef, advpng, pngcrush -reduce) are lossless and safe.
"""

import argparse
import sys

import numpy as np
from PIL import Image


def gaussian_kernel(sigma, radius):
    """A square Gaussian kernel, unnormalised.

    Normalisation is pointless here: every decision the algorithm makes is an argmin or an
    argmax over the energy array, and both are invariant to a positive scale factor.
    """
    axis = np.arange(-radius, radius + 1)
    xx, yy = np.meshgrid(axis, axis)
    return np.exp(-(xx * xx + yy * yy) / (2.0 * sigma * sigma))


def stamp(energy, kernel, position, sign):
    """Add (sign=+1) or remove (sign=-1) one point source's contribution, wrapping at the edges.

    Wrapping here is the whole reason the finished tile is seamless. The modular index arrays
    are only correct while the kernel is narrower than the tile, which build_tile() enforces.
    """
    radius = kernel.shape[0] // 2
    y, x = position
    rows = np.arange(y - radius, y + radius + 1) % energy.shape[0]
    cols = np.arange(x - radius, x + radius + 1) % energy.shape[1]
    energy[np.ix_(rows, cols)] += sign * kernel


def tightest_cluster(energy, binary):
    """Location of the 1 with the most 1s around it."""
    return np.unravel_index(np.argmax(np.where(binary, energy, -np.inf)), energy.shape)


def largest_void(energy, binary):
    """Location of the 0 with the fewest 1s around it."""
    return np.unravel_index(np.argmin(np.where(binary, np.inf, energy)), energy.shape)


def initial_pattern(size, kernel, density, rng, max_swaps):
    """Ulichney's initial binary pattern: scatter, then relax to maximum homogeneity.

    Repeatedly move the point from the tightest cluster into the largest void. Convergence is
    when the void that opens up is the one just vacated, i.e. the move is a no-op.
    """
    total = size * size
    ones = max(1, int(round(total * density)))

    binary = np.zeros((size, size), dtype=bool)
    flat = rng.choice(total, size=ones, replace=False)
    binary.flat[flat] = True

    energy = np.zeros((size, size), dtype=np.float64)
    for position in zip(*np.nonzero(binary)):
        stamp(energy, kernel, position, +1)

    for swap in range(max_swaps):
        cluster = tightest_cluster(energy, binary)
        binary[cluster] = False
        stamp(energy, kernel, cluster, -1)

        void = largest_void(energy, binary)
        if void == cluster:
            binary[cluster] = True
            stamp(energy, kernel, cluster, +1)
            return binary, energy, swap

        binary[void] = True
        stamp(energy, kernel, void, +1)

    raise RuntimeError(
        'initial pattern did not converge in {} swaps'.format(max_swaps))


def build_tile(size, sigma, density, seed, max_swaps, progress=None):
    """Return the rank array: every value in 0 .. size*size-1 exactly once."""
    radius = int(np.ceil(4.0 * sigma))
    if 2 * radius + 1 > size:
        radius = size // 2 - 1
    if radius < 1:
        raise ValueError('tile too small for sigma {}'.format(sigma))
    kernel = gaussian_kernel(sigma, radius)

    rng = np.random.default_rng(seed)
    total = size * size
    prototype, prototype_energy, swaps = initial_pattern(
        size, kernel, density, rng, max_swaps)
    ones = int(prototype.sum())
    if progress:
        progress('initial pattern: {} points, converged after {} swaps'.format(ones, swaps))

    rank = np.zeros((size, size), dtype=np.int64)

    # Phase 1 -- ranks below the initial pattern's population. Strip the prototype back to
    # nothing, tightest cluster first; the last point standing is the most isolated and gets
    # rank 0.
    binary = prototype.copy()
    energy = prototype_energy.copy()
    for r in range(ones - 1, -1, -1):
        cluster = tightest_cluster(energy, binary)
        binary[cluster] = False
        stamp(energy, kernel, cluster, -1)
        rank[cluster] = r

    # Phases 2 and 3 -- ranks from the initial population up. Fill the prototype in, largest
    # void first, until every pixel is set.
    #
    # Ulichney splits this in two at the halfway point, reversing the roles of void and cluster
    # for the second half. On a torus that reversal is a no-op and the two phases collapse into
    # this single loop: the wrapped kernel makes the sum of all shifted kernels spatially
    # uniform, so the energy of the 1s and the energy of the 0s add to a constant. Maximising
    # the latter over the unset pixels is therefore exactly minimising the former over them,
    # which is what largest_void() already does. Truncating the kernel does not disturb this --
    # the sum is still the same constant everywhere.
    binary = prototype.copy()
    energy = prototype_energy.copy()
    for r in range(ones, total):
        if progress and r % 4096 == 0:
            progress('filling: {}/{}'.format(r, total))
        void = largest_void(energy, binary)
        binary[void] = True
        stamp(energy, kernel, void, +1)
        rank[void] = r

    if sorted(rank.flat) != list(range(total)):
        raise RuntimeError('rank array is not a permutation -- generator bug')
    return rank


def to_codes(rank, bits):
    """Map ranks to output codes, keeping every code equally frequent for power-of-two tiles."""
    total = rank.size
    levels = 1 << bits
    codes = (rank.astype(np.int64) * levels) // total
    return np.minimum(codes, levels - 1)


def radial_spectrum(binary):
    """Radially averaged power spectrum of a binary pattern, indexed by integer radius."""
    centred = binary.astype(np.float64) - binary.mean()
    power = np.abs(np.fft.fftshift(np.fft.fft2(centred))) ** 2

    size = binary.shape[0]
    axis = np.arange(size) - size // 2
    xx, yy = np.meshgrid(axis, axis)
    radius = np.round(np.sqrt(xx * xx + yy * yy)).astype(int)

    counts = np.bincount(radius.ravel())
    sums = np.bincount(radius.ravel(), weights=power.ravel())
    return sums[counts > 0] / counts[counts > 0]


def verify(rank, quiet):
    """Report whether the tile actually looks like blue noise.

    Blue noise means the energy sits at high spatial frequencies with a hole at low ones. We
    slice the rank array at several thresholds, because a dither array is used at every
    threshold and each slice has to be well distributed on its own.
    """
    size = rank.shape[0]
    total = rank.size
    nyquist = size // 2
    ok = True

    for fraction in (0.25, 0.50, 0.75):
        spectrum = radial_spectrum(rank < fraction * total)
        # "Low" is the first eighth of the band up to Nyquist -- the region a blue-noise
        # pattern is supposed to leave empty. White noise would put an eighth of its energy
        # there; clumpy (red) noise, much more.
        cut = max(1, nyquist // 8)
        low = spectrum[1:cut + 1].sum()
        allband = spectrum[1:nyquist + 1].sum()
        share = low / allband
        peak = int(np.argmax(spectrum[1:nyquist + 1])) + 1
        flat = cut / float(nyquist)
        good = share < 0.5 * flat and peak > cut
        ok = ok and good
        if not quiet:
            print('  threshold {:>4.0%}: low-band energy {:6.2%} (white noise would be '
                  '{:.2%}), peak at radius {}/{} -- {}'.format(
                      fraction, share, flat, peak, nyquist, 'ok' if good else 'SUSPECT'))
    return ok


def write_png(path, codes, bits):
    dtype = np.uint8 if bits == 8 else np.uint16
    Image.fromarray(codes.astype(dtype)).save(path)


def write_spectrum_png(path, rank):
    """Log power spectrum of the 50% slice, for eyeballing the low-frequency hole."""
    centred = (rank < rank.size // 2).astype(np.float64)
    centred -= centred.mean()
    power = np.abs(np.fft.fftshift(np.fft.fft2(centred))) ** 2
    image = np.log1p(power)
    top = image.max()
    if top > 0:
        image = image / top
    Image.fromarray((image * 255).astype(np.uint8)).save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('-o', '--output', default='data/shaders/blue_noise_64.png',
                        help='output PNG path (default: %(default)s)')
    parser.add_argument('-s', '--size', type=int, default=64,
                        help='tile edge in pixels, power of two (default: %(default)s)')
    parser.add_argument('--sigma', type=float, default=1.5,
                        help="Gaussian filter sigma, Ulichney's value (default: %(default)s)")
    parser.add_argument('--density', type=float, default=0.1,
                        help='fraction of pixels in the initial pattern (default: %(default)s)')
    parser.add_argument('--seed', type=int, default=20260829,
                        help='RNG seed; the output is a function of this (default: %(default)s)')
    parser.add_argument('--bits', type=int, choices=(8, 16), default=8,
                        help='output bit depth (default: %(default)s)')
    parser.add_argument('--max-swaps', type=int, default=1000000,
                        help='give up if the initial pattern will not settle')
    parser.add_argument('--spectrum', metavar='PATH',
                        help='also write the log power spectrum of the 50%% slice here')
    parser.add_argument('--no-verify', action='store_true',
                        help='skip the blue-noise check')
    parser.add_argument('-q', '--quiet', action='store_true')
    args = parser.parse_args()

    if args.size & (args.size - 1):
        parser.error('--size must be a power of two')

    progress = None if args.quiet else (lambda text: print('  ' + text, flush=True))
    if not args.quiet:
        print('Generating {0}x{0} void-and-cluster tile (sigma {1}, seed {2})'.format(
            args.size, args.sigma, args.seed))

    rank = build_tile(args.size, args.sigma, args.density, args.seed, args.max_swaps, progress)

    if not args.no_verify:
        if not args.quiet:
            print('Verifying:')
        if not verify(rank, args.quiet):
            print('error: the generated tile does not look like blue noise', file=sys.stderr)
            return 1

    write_png(args.output, to_codes(rank, args.bits), args.bits)
    if not args.quiet:
        print('Wrote {}'.format(args.output))

    if args.spectrum:
        write_spectrum_png(args.spectrum, rank)
        if not args.quiet:
            print('Wrote {}'.format(args.spectrum))
    return 0


if __name__ == '__main__':
    sys.exit(main())
