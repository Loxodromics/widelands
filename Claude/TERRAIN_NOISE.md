# Terrain Variation: Layered Simplex Noise

Design for breaking up the visible repetition in terrain rendering, using procedural noise
evaluated in the fragment shader rather than a baked overlay texture.

Companion documents: `RENDERER.md` (how the renderer is put together),
`widelands-visual-fidelity-ideas.md` (the wider brainstorm this was selected from).

---

## 1. The defect, stated precisely

Terrain UVs are a **world-space projection**, not per-tile coordinates. In
`fields_to_draw.cc:145-148`:

```cpp
Vector2f map_pixel = MapviewPixelFunctions::to_map_pixel_ignoring_height(f.geometric_coords);
f.texture_coords.x =  map_pixel.x / Widelands::kTextureSideLength;
f.texture_coords.y = -map_pixel.y / Widelands::kTextureSideLength;
```

with `kTextureSideLength = 64` (`terrain_description.h:36`), and a load-time check that rejects
any terrain texture that is not exactly 64x64 (`terrain_description.cc:242`). The isometric
constants are `kTriangleWidth = 64`, `kTriangleHeight = 32`
(`ui/wui/mapviewpixelconstants.h:27-29`).

Substituting `map_pixel.x = 64 * fx + 32 * (fy & 1)` and `map_pixel.y = 32 * fy` gives the
texture coordinate at a node:

```
u =  fx + 0.5 * (fy & 1)
v = -fy / 2
```

Three consequences follow, and they drive every decision below.

**(a) The repetition period is one field wide and two rows tall** — a 64x64 pixel lattice on
screen at zoom 1. The tiling is *seamless* (adjacent triangles of the same terrain are
continuous, because the UV is a continuous function of world position), but the period is small
enough that the eye reads it as wallpaper. This, not the absence of shading, is the dominant
cause of the flat look. Terrain is already Gouraud-shaded from a real surface normal
(`field.cc:29-83`, interpolated via `attr_brightness` in `terrain.vp:4` and `terrain.fp:24`).

**(b) The noise input coordinate is, to within a half-field offset, the field coordinate
itself.** `u` counts fields, `v` counts half-rows. This is convenient: frequency `f` in that
space has wavelength `1/f` *fields*, so parameters read directly as "patches N fields across".

**(c) The coordinate space is isotropic in screen pixels.** Both `u` and `v` are divided by the
same constant 64, and `map_pixel` is already in screen pixels. So `snoise(uv)` yields round
blobs on screen, not blobs stretched along the isometric axes. No aspect correction is needed.

## 2. Why a shader, not a texture

The doc's §2.4 proposed a tileable grunge texture multiplied over the terrain. Against this
renderer, procedural noise is strictly better:

- **No atlas problem.** Terrain textures must land in the *first* texture atlas, and this is
  asserted at startup (`build_texture_atlas.cc:113-159`, "Not all images that should fit in the
  first texture atlas did actually fit"). A separate noise texture would need its own texture
  unit and its own `GL_REPEAT` wrap mode — `Texture` currently sets `GL_CLAMP_TO_EDGE`
  unconditionally (`texture.cc`), so it would need a parameter. Doable, but it is real plumbing.
- **A tiled texture reintroduces a period**, just a longer one. The whole point is to remove
  periodicity.
- **Non-repeating detail at any frequency**, tunable by editing a constant instead of
  regenerating an asset.

The cost is ALU per fragment, quantified in §7.

## 3. Noise choice

**2D simplex, the Ashima Arts / Ian McEwan implementation** (`webgl-noise`). Reasons:

- Pure float math — `floor`, `fract`, `dot`, `max`, `abs`, `step`. No bitwise operators, no
  integer ops, no texture lookups. We target **GLSL 1.20** (`initialize.cc:44-45,260-261`; every
  shader in `data/shaders/` opens with `#version 120`), which has none of those. Function
  overloading on `vec2`/`vec3` is available in 1.20, so the `mod289` overloads are fine.
- MIT licensed, which is GPL-compatible. The attribution header must be kept in the file.
- The Simplex patent covered 3D and higher and expired in 2022 in any case.

Value noise would be cheaper but has visible axis-aligned artefacts, which is exactly the
failure mode we are trying to remove.

## 4. Per-fragment or per-vertex

Both are viable and the choice is worth making deliberately.

| | Per-vertex (in `terrain.vp`, interpolated) | Per-fragment (in `terrain.fp`) |
|---|---|---|
| Evaluations/frame | one per visible node, a few thousand | one per terrain pixel, ~2M at 1080p |
| Frequency limit | wavelengths >> 1 field only | any |
| C++ changes | none | none |
| Iteration | same | same |

Per-vertex is nearly free, but it samples the noise on the field lattice — the exact lattice
whose visibility is the problem. Any octave with a wavelength near one field would alias into
that lattice, so the octave that does the most useful work is the one it cannot represent.

**Decision: per-fragment.** Keep per-vertex in reserve as the low-spec fallback (§7), where
dropping the finest octave is acceptable anyway.

## 5. Layering

Three octaves, with a rotation applied between each so the simplex lattice axes never line up
across octaves:

```glsl
// ~37 degrees; any angle that is not a multiple of 30 will do.
const mat2 kOctaveRotation = mat2(0.80, 0.60, -0.60, 0.80);
```

| Octave | Frequency | Wavelength | Amplitude | Role |
|---|---|---|---|---|
| 1 | 0.09 | ~11 fields | 1.00 | Regional patchiness: a dry stretch, a lush stretch. The AoE "this area differs from that area" read. |
| 2 | 0.21 | ~4.8 fields | 0.50 | Mid-scale mottling. |
| 3 | 0.55 | ~1.8 fields | 1.20 | Maximum tonal contrast between neighbouring repeats. Carries most of the budget on purpose — see the antiphase rule below. |

Normalised by the sum, 2.70.

### The antiphase rule

**This governs octave choice for value modulation. Read it together with §16, which shows what it
does and does not buy: it maximises the tonal difference between neighbouring repeats, but it does
not make the repeating pattern itself any less visible.**

The repetition to be broken has a period of exactly one field (§1). Two points one period apart
are separated most strongly by a wave whose own wavelength is *twice* that separation — at
λ = 2 fields the two points sit in antiphase. So the octave that does the work is at **f ≈ 0.5
cycles per field**, and there is no benefit in going finer:

- f ≈ 0.5 (λ = 2 fields): adjacent repeats land in antiphase. Maximum separation.
- f ≈ 1.0 (λ = 1 field): adjacent repeats land back *in phase*. Nearly zero separation — the
  octave aliases with the lattice and does almost nothing.

Measured on `Finnish_Lakes` at view 640,640,1.0, as RMS relative-luminance difference between
pixels exactly one field (64 px) apart, against the total field sd:

| Weighting | field sd | adjacent-tile RMS | % of fully decorrelated |
|---|---|---|---|
| 0.25 @ f=0.55 (first attempt) | 2.60% | 1.42% | 39% |
| + a 4th octave at f=1.10 | 2.31% | 1.59% | 49% |
| **1.20 @ f=0.55 (current)** | **2.59%** | **2.59%** | **71%** |

Adding a higher-frequency octave barely helped, exactly as the rule predicts. Moving amplitude
into the f≈0.55 octave nearly doubled the adjacent-tile difference at unchanged total sd — the
same visual loudness, much better aimed.

Calibration note from the references: in `referenceImages/AoE2_0.png` the grass variation is
dominated by the *large* scales, with fine detail supplied by scattered clutter rather than by the
ground texture. That argues for keeping octave 1 present, which it is — but it must not come at
the cost of the f≈0.5 octave, because the two are solving different problems: octave 1 buys
regional interest, octave 3 buys local tone-to-tone contrast. Neither buys an end to the
wallpaper pattern; see §16 for what that needs.

## 6. What gets modulated

**Phase 1 — value only.** A multiplicative brightness factor, which composes correctly with the
existing shading and with fog of war:

```glsl
float n = terrain_fbm(var_texture_position);        // approx [-1, 1]
clr.rgb *= var_brightness * (1.0 + kValueAmplitude * n);
```

Starting point `kValueAmplitude = 0.07` (+/-7%). Much beyond 0.10 stops reading as terrain
variation and starts reading as mould.

**Deviations from this document, agreed at implementation time:**

1. The original Phase 1 was "shader only, no C++". Editor captures are the only way to see a whole
   map without fog of war, and `ui/editor/editorinteractive.cc:190-191` unconditionally sets
   `dfShowGrid | dfShowResources` after construction, overriding config. A grid on exactly the
   lattice we are trying to disrupt makes the result unjudgeable, so a small harness change was
   included: capture mode clears both flags (`src/dev_harness/capture.cc`). It lands in the dev
   harness, not the renderer, so the *rendering* change is still pure data.
2. `dither.fp` was supposed to wait for Phase 1b. Leaving it out puts a brightness seam along every
   terrain border, competing with the effect during parameter tuning. The noise function is
   duplicated into `dither.fp`, marked as temporary; Phase 1b removes the duplication via the shared
   `noise.glsl` and the `#include` mechanism (see §8).

A third finding during implementation: the editor's gametime advances by wall clock
(`EditorInteractive::think()`), so animated water and immovables made editor captures
non-reproducible. Capture mode now pins the editor gametime (same harness change as deviation 1).
See "Phase 1 results".

**Phase 1b — a warm/cool axis.** A pure brightness multiply reads as *lighting*, not as
*material*. Real ground variation shifts hue as well: drier patches are yellower, shaded growth
is cooler and greener. A second field driving a small tint fixes this cheaply:

```glsl
clr.rgb *= mix(vec3(1.0), kWarmTint, kTintAmplitude * t);   // kWarmTint = vec3(1.06, 1.00, 0.92)
```

With `t` in [-1, 1] the `mix` extrapolates below zero, giving a cool shift on one side and a warm
shift on the other from a single term. Starting point `kTintAmplitude = 0.4`.

To avoid a second full fBm evaluation, derive `t` from a different weighting of the same three
octaves rather than sampling a fresh offset field. The two outputs are then correlated rather
than independent, which is a real if minor compromise — value and hue will trend together. If
that reads badly, a fourth octave sampled at an offset costs one more `snoise` call.

## 7. Cost, and the low-spec question

The Ashima 2D simplex is roughly 60-70 GPU instructions. Three octaves plus the plumbing is on
the order of 200 instructions per fragment. Full-screen terrain at 1920x1080 is ~2.07M
fragments, so ~420M instructions per frame, ~12.6 G/s at the 30 FPS cap
(`kDrawDelay = 1000/30`, `panel.cc:200`).

Overdraw is not a concern: terrain base is drawn opaque with `BlendMode::Copy` and a depth test
(`game_renderer.cc`, `render_queue.cc:214`), so each pixel is shaded once, plus the dither pass
over transition triangles only.

On anything from roughly the last decade this is not measurable — an Intel HD 4000 is around 300
GFLOPS. On the genuinely old hardware that the deliberate GL 2.1 target implies, it is not free.
Widelands targets 2.1 for compatibility on purpose, so **a config toggle belongs in phase 2**,
not as an afterthought. The per-vertex variant from §4 is the natural "low" setting.

## 8. The dither pass must match

`DitherProgram` repaints transition triangles with the neighbouring terrain's texture, masked by
the dither alpha (`dither.fp:16-24`). It carries the *same* `var_texture_position` varying, so
applying the identical noise function there yields the identical value at the same world
position — which is exactly what correctness requires. Omit it and every terrain border gets a
visible discontinuity in the variation.

GLSL 1.20 has no `#include`, so the function has to be shared somehow. `Program::build`
(`gl/utils.cc:146-148`) reads each stage as a `std::string` before compiling:

```cpp
std::string fragment_shader_source = read_file("shaders/" + program_name + ".fp");
```

A single-level textual `#include` expansion in `read_file` is about 15 lines and lets
`terrain.fp` and `dither.fp` both pull in a shared `data/shaders/noise.glsl`. That is the right
answer and it pays off for every shader we touch afterwards.

**Current state:** `dither.fp` carries a byte-identical duplicate of the noise block from
`terrain.fp` (plus a TODO pointing here), so terrain borders are continuous in Phase 1. Removing
that duplicate by landing `noise.glsl` + the `#include` expansion is the concrete Phase 1b task —
the duplication is temporary and deliberately so.

## 9. Interactions checked

**Zoom and aliasing.** Zoom range is 1/4 to 4 with `scale = 1/zoom`, so a field spans between 16
and 256 screen pixels. The finest octave has a wavelength of ~1.8 fields, i.e. ~29 screen pixels
at maximum zoom-out. Aliasing would need it under ~4 pixels, so we have an order of magnitude of
headroom and **no `fwidth`-based octave fading is required**. Worth revisiting only if the zoom
range widens or a much finer octave is added.

**Fog of war.** FoW is applied as a per-node brightness override
(`interactive_player.cc:66-87`, `:552`), so the noise multiplies on top and needs no special
case. Unexplored fields have brightness 0, where the noise is invisible by construction.

**Torus wrap.** `texture_coords` derives from `geometric_coords`, which is *not* normalised
(`fields_to_draw.cc:145-148`; normalisation happens separately at `:150-152` for `fcoords`). So
the noise coordinate increases monotonically as you scroll and never wraps — there is no seam.
The residual artefact is that a field visible *twice at once* would show different variation in
each copy while its terrain texture matches. That needs the map to be narrower than the
viewport, which is reachable: a 64-field map is 4096 world pixels wide against 7680 visible at
maximum zoom-out.

If it turns out to matter, periodic simplex on normalised coordinates fixes it exactly. All map
dimensions are multiples of 16 (`map.h:57-59`), so the period `(W, H/2)` is always a whole number
of lattice cells and the `0.5 * (fy & 1)` row offset is preserved across the wrap. Not worth
doing up front.

**Determinism.** The noise is a pure function of world position, so captures stay byte-identical
and `wl.py --compare` keeps working.

**Unaffected.** Minimap (`minimap_renderer.cc` uses averaged terrain colours), workarea overlays
(`workarea.fp` is a flat colour), the height heat map (a separate `FillRectProgram` path), and
the editor's terrain picker (raw texture thumbnails).

**Roads** sample their own texture with their own UV (`road.fp`), so they stay clean while the
ground around them varies. Probably fine — roads are worn surfaces — but it is a thing to look
at in the first captures.

## 10. Phasing

**Phase 1 — shader only, no C++.** Add simplex plus a three-octave fBm to `terrain.fp`,
brightness modulation only, constants hard-coded. Evaluate on real scenes. Revert cost is
`git checkout data/shaders/terrain.fp`.

**Done 2026-08-13**, with the two deviations folded into §6: a capture-mode harness change
(clean editor overlays, pinned editor gametime) and the temporary duplicate in `dither.fp`.
Parameter values as designed: amplitude 0.07, frequencies 0.09 / 0.21 / 0.55 with the 37°
rotation. Results in "Phase 1 results".

**Phase 1b — get it right.** Add the warm/cool axis. Add the `#include` expansion to
`Program::build` and wire `dither.fp` to the shared `noise.glsl`, removing the temporary
duplicate.

**Phase 2 — make it controllable.** Amplitudes as uniforms, plus a config option so it can be
switched off. `scale` is already carried in `terrain_arguments` (`game_renderer.cc`) but is not
currently passed down to `TerrainProgram::draw` (`render_queue.cc:266`), so any zoom-dependent
uniform needs that plumb-through first.

**Phase 3 — per terrain type, only if needed.** A single global amplitude will not suit snow,
lava, water and meadow equally. Terrain type is a per-triangle property, and the program already
replicates a per-triangle value to all three vertices (`texture_offset`,
`terrain_program.cc:110-125`), so an `attr_noise_amplitude` follows the identical pattern at 4
bytes per vertex, fed from a new optional field in the Lua terrain definitions. Do not start
here — decide it from screenshots.

## 11. Verification

The harness gives deterministic capture (`Claude/wl.py`, `DEV_HARNESS.md`), but everything so
far is validated against `plain.wmf`, which exercises almost none of this. **Backlog item 1.7
(scene catalog) is a prerequisite in practice**, not because comparison is automated — judging
this is eyeballing, not pixel diffing — but because we need a scene containing meadow, steppe,
desert, water, mountain and some real slopes to judge *against*.

Capture each scene at zoom 1, 2 and 0.5. Note that this changes every terrain pixel, so all
existing terrain baselines are invalidated by design and need recapturing once the parameters
settle.

## 12. Open questions

**Caveat on the answers below:** they were measured before the §14 review, i.e. on captures that
still carried the build-help overlay and with the pre-retune octave weights. The directions still
hold, the numbers are stale.

- Does the large-scale octave fight the existing per-terrain art, which already carries its own
  baked colour variation? Possible that octave 1 wants to be weaker than the AoE reference
  suggests. — **Measured, not settled.** At zoom 2.0 the octave-1-scale component is 5-7% of the
  difference image's variance (it is dominated by texture-grain-scale modulation instead); the
  large patches are present but subtle. Whether they fight the baked variation is an eyeball call
  on the Phase 1 captures — still open.
- Water is a terrain like any other and is already frame-animated (33 frames at 14 fps,
  `data/world/terrains/summer/water/init.lua`). Static positional noise on top of animated water
  may read as dirt on the screen. — **Open, with data.** Water receives the full amplitude
  (measured mean per-pixel change on Finnish Lakes: 7.8 vs 2.2 on land, in 8-bit sum-of-absolute
  RGB deltas — proportional to water being brighter). Whether it reads as dirt is a visual call;
  this is the evidence for Phase 3 (per-terrain amplitude), not for a Phase 1 fix.
- Does the variation survive at zoom 4 (zoomed out), where a field is 16 pixels and the player is
  looking at regional structure rather than ground texture? That is the view where octave 1
  should be doing the most work. — **Partially answered.** At zoom 2.0 (field 32 px) the variation
  is present at mean |ΔL| ≈ 4-6. Zoom 4.0 was not part of the capture matrix; the octave-1 share
  at 2.0 (5-7%, see above) suggests it survives but does not dominate. Add a zoom-4 view to the
  next capture round if this matters.

## 13. Phase 1 results

Landed as planned in §10 plus the §6 deviations. The capture matrix ran on four maps
(`Finnish_Lakes`, `The_Nile`, `Dolomites`, `Glacier_Lake`) at zooms 0.5 / 1.0 / 2.0, editor mode,
view 640,640, deterministic (two runs per map byte-identical; `wl.py --compare`).

**What the captures showed:**

- The shaders compile (any capture succeeding after the change proves it; a GLSL error throws at
  program build during graphics init).
- The effect is everywhere and subtle-to-moderate: 60-92% of pixels change per view; per-pixel
  |ΔL| up to ±12 with the mean near zero (−0.1 to −2.4, slight darkening bias from the fBm not
  being perfectly symmetric). Nothing suggests the "mould" failure mode at 0.07.
- At the pixels where the baseline texture repeated exactly (the wallpaper symptom, texel pairs
  matching at lag 64), the noise version now differs by ~1.2 mean luminance — i.e. the repetition
  envelope is changed, but whether the lattice *reads* is a judgement call on the captures. The
  plan's tuning lever (bump octave 3, the direct lattice breaker) is available if it still reads.
- Terrain borders showed no seam: `terrain.fp` and `dither.fp` carry the identical function and
  the dither pass samples the same world position, so the values agree by construction (verified
  as identical code text; the dither band changes with the rest of the terrain in the captures).
- The editor-gametime problem this phase uncovered: `EditorInteractive::think()` advances
  gametime by wall clock, so water and immovable animation made editor captures differ between
  runs along every shoreline dither band (5.4% of pixels on Finnish Lakes at view 640,640; the
  map interior was already deterministic). Capture mode now pins the editor gametime
  (`editorinteractive.cc`, gated on `DevHarness::capture_enabled()`), which made all four maps
  byte-identical across runs.
- Water takes the full amplitude, see §12.

**What got tuned:** nothing, on the first pass. The eyeball call was deferred, and that turned out
to matter — see §14.

**What is now known that was not at design time:**

- The editor's gametime claim in the original design (§10, and `capture.cc`) was wrong — it
  advances by wall clock. Fixed in the harness, recorded in `DEV_HARNESS.md`.
- The change is dominated by texture-grain-scale modulation (the product of per-pixel brightness
  with the noise field), not by the octave structure — worth remembering when judging the
  captures: the per-pixel texture shimmer is expected at this amplitude.
- Roads (`road.fp`) and minimap are unaffected, as designed; whether roads now look "cleaner" than
  their surroundings is on the eyeball list.

## 14. Review of Phase 1, and the retune

The statistics in §13 were gathered but the visual judgement was not made. Making it turned up two
problems.

**The captures were not clean.** Clearing `dfShowGrid` and `dfShowResources` was not enough:
`EditorInteractive::map_changed()` calls `show_buildhelp(true)` *after* the constructor sets the
display flags, so every capture carried a build-help symbol on nearly every field — the same
per-field lattice the terrain work is judged against. Fixed in the harness; §11's capture recipe
only produces usable images from that commit onward. Any conclusion drawn from an editor capture
taken before it should be re-checked.

**The parameters could not do the job they were chosen for.** With 0.25 amplitude on the f=0.55
octave, the noise field was a smooth wash: sd 2.6% relative luminance, but only 1.42% RMS between
pixels one field apart — 39% of what two uncorrelated points would show, with full decorrelation
only at four fields' distance. Rendering the difference field amplified made it obvious: blobs
several hundred pixels across, essentially no structure at the 64 px repeat. Phase 1 as first
landed delivered *regional tonal variation*, which is worth having, but left the wallpaper
symptom untouched.

The cause was a wrong claim in §5 — that the f=0.55 octave "directly disrupts the 1-field
lattice". The frequency was right; its share of the amplitude budget was not. §5 now carries the
antiphase rule and the measurements behind it, and the octave weights are 1.00 / 0.50 / 1.20
normalised by 2.70, which takes adjacent-tile decorrelation from 39% to 71% at unchanged total sd.

**Amplitude is the remaining open call.** At `kValueAmplitude = 0.07` the retuned field is still
near the threshold of perception on a 1280x720 capture. A test at 0.12 makes the mottling plainly
visible on meadow and steppe without reading as dirt, which suggests the "beyond 0.10 looks like
mould" guess in §6 was pessimistic — it was made when the amplitude was spread across the wrong
scales, where a large value would have shown up as broad blotches rather than as texture. Left at
0.07 pending an aesthetic decision; raising it is a one-line change in both shaders.

**Method note for later phases:** the useful measurement here was not mean pixel change, which
says only that something happened. It was the RMS difference between points exactly one repeat
apart, normalised by the field's own sd. That is what distinguishes "the noise is loud" from "the
noise is aimed at the defect", and the two came apart badly on the first attempt.

## 15. Tuning: where the knobs are

Everything below lives in `data/shaders/terrain.fp`, and **must be mirrored byte-for-byte into
`data/shaders/dither.fp`** until Phase 1b lands the shared `noise.glsl` — the two shaders have to
compute the same value at the same world position or every terrain border grows a seam.

| Knob | `terrain.fp` | Current | What it does |
|---|---|---|---|
| Overall strength | `:70` `kValueAmplitude` | `0.07` | Peak brightness swing as a fraction. The only knob that changes how loud the effect is. |
| Octave 1 frequency | `:62` `snoise(p * 0.09)` | `0.09` | Regional patches, ~11 fields across. Lower = broader. |
| Octave 2 | `:64` `0.50 * snoise(p * 0.21)` | amp `0.50`, f `0.21` | Mid-scale mottling, ~4.8 fields. |
| Octave 3 | `:66` `1.20 * snoise(p * 0.55)` | amp `1.20`, f `0.55` | The one aimed at the 1-field repeat. Do not raise its *frequency* — see the antiphase rule in §5. |
| Normaliser | `:67` `sum / 2.70` | `2.70` | Must equal the sum of the three amplitudes, or the effective strength drifts away from `kValueAmplitude`. |
| Inter-octave rotation | `:61` `mat2(0.80, 0.60, -0.60, 0.80)` | ~37° | Keeps the simplex lattice axes from lining up across octaves. No reason to touch it. |

Frequencies are in **cycles per field**, because the input `var_texture_position` is world position
in field units (§1). So wavelength in fields is `1 / f`, and a patch "N fields across" is `f = 1/N`.

**Iteration loop — no rebuild.** `wl.py` passes `--datadir=<repo>/data`, so shaders are read from
the source tree at run time. Edit the `.fp` files and re-run:

```
Claude/wl.py --editor data/maps/Finnish_Lakes.wmf --view 640,640,1.0 --shot /tmp/out.png
```

Zoom is `scale = 1/zoom`: **1.0 native, 0.5 zoomed in, 2.0 zoomed out**. A GLSL error throws at
program build and `--fail-on-errors` turns it into a non-zero exit, with the info log next to the
requested screenshot path. `git checkout HEAD -- data/shaders/` reverts.

**Two measurements worth taking, because they answer different questions.** Both operate on a
capture pair (baseline vs. variant), and both were needed to see what was actually going on:

1. *Is the noise aimed at the repeat?* RMS difference of the relative field between pixels exactly
   one field apart, normalised by the field's own sd. Mean pixel change does not answer this.
2. *Did the repeat actually weaken?* High-pass the image (subtract a box blur of about half a
   field), then take the correlation at a 1-field lag. Validate the metric first by sweeping the
   lag — it should peak at 1 and 2 fields and sit near zero in between, as it does.

## 16. What the amplitude ladder showed

A ladder at `kValueAmplitude` 0.07 / 0.15 / 0.25 / 0.40 was captured to see the effect clearly and
calibrate down from there. It answered the amplitude question and invalidated a larger assumption.

**The effect is real and reads well.** At 0.40 the meadow and steppe carry obvious soft patches
that look like variable grass health or drifting cloud shadow. Nothing about it reads as dirt or
mould even at 5.7x the shipped strength, so the §6 warning about 0.10 was simply wrong.

**But the repetition is untouched at every amplitude.** Measured on the cobble/steppe region of
`Finnish_Lakes` at zoom 0.5, high-passed so only the pattern is measured:

| | relative-field sd | pattern self-similarity at 1-field lag |
|---|---|---|
| baseline, no noise | — | 0.0618 |
| amp 0.07 | 2.13% | 0.0623 |
| amp 0.40 | 10.36% | 0.0636 |

The tonal field grew fivefold; the repeat did not weaken at all — it drifted marginally *up*. The
zoom-0.5 capture at 0.40 shows the same thing directly: the cobble motif still tiles visibly under
a strong tonal wash.

**Why, and it should have been obvious from the start.** A multiplicative brightness field is
smooth over a field or more, so two adjacent repeats get *the same pattern at slightly different
exposure*. The eye reads repetition from the pattern, not the brightness level. Value modulation
therefore cannot fix the §1 defect at any amplitude — the antiphase retune in §14 maximised the
wrong quantity correctly.

**What this means for the roadmap.** These are two separate problems and were conflated throughout
this document:

- *Tonal uniformity* — flat, dead colour over large areas. Value noise fixes this, and is the
  feature we now have. It wants an amplitude around 0.15-0.25 on this evidence, not 0.07.
- *Pattern repetition* — the 64 px wallpaper lattice. Needs the sampled texel to change, not its
  brightness. Cheapest next experiment is **domain warping**: perturb `var_texture_position` by a
  small noise offset *before* the `fract()` in both shaders, which distorts the pattern itself and
  stays a shader-only change. Risk is wobbling hard-edged terrains such as cobble, so it likely
  wants a small amplitude and possibly per-terrain control (Phase 3). The other routes are per-tile
  texture variants (rejected in the ideas doc §2.3, it breaks UV continuity) and the scatter layer
  (ideas doc §2.6), which sidesteps the problem by covering the ground.
