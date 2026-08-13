# Widelands Visual Fidelity: Improvement Ideas

## Intro & Constraints

Goal: figure out how Widelands' visual fidelity could be pushed closer to something like AoE2:
Definitive Edition, using AoE (and other strategy games) as inspiration for concrete techniques.
Reference screenshots live in `referenceImages/`.

Constraints that shaped these ideas:

- I'm a **programmer**. Code changes (shaders, render pipeline, procedural generation) are cheap for me.
- I *can* do some artist work, but I **prefer programming work** and want to minimize hours spent painting/modeling.
- **Personal time is the biggest constraint** — not money, not team size. Every idea below is implicitly ranked with "how many of my hours does this cost" in mind, favoring code/automation over manual art wherever possible.

**Status of this document:** originally written against possibly-outdated screenshots, with every
claim about what Widelands "currently does" flagged as a hypothesis. Those hypotheses were checked
against the source on 2026-08-13; the results are folded in below and marked. Several were wrong,
and the corrections reorder the priorities substantially.

Companion documents: `RENDERER.md` (how the renderer is put together), `TERRAIN_NOISE.md` (worked-out
design for the first item), `backlog.md` (what is actually scheduled).

---

## 0. Verification Summary

**Already implemented — nothing to do:**

| Assumed missing | Reality |
|---|---|
| Gouraud shading (§2.1) | Brightness is a per-vertex attribute, interpolated across triangles: `fields_to_draw.cc:163`, `terrain.vp:4`, `terrain.fp:24`. |
| A real lighting model | `Field::set_brightness` (`field.cc:29-83`) builds a surface normal from six neighbour height deltas and dots it with a fixed sun vector `(0.577, -0.577, -0.577)`. |
| Animated water (§5.3) | Already frame-animated: 33 frames at 14 fps (`data/world/terrains/summer/water/init.lua`), swapped per frame by `TerrainDescription::get_texture(gametime)`. |
| Unit movement smoothing (§5.9) | `Bob::calc_drawpos` (`bob.cc:729-802`) LERPs between nodes by elapsed walk time, including a height term and a parabolic arc over bridges. |
| Soft fog-of-war edges (§5.5) | FoW is a per-node brightness override (`interactive_player.cc:66-87`, `:552`) fed through the same vertex interpolation, so the edge already fades across a full triangle. Only its *outline* is grid-shaped. |

**Confirmed missing:**

- No drop shadows anywhere. The only "shadow" in `src/graphic` is a text-rendering style flag
  (`style_manager.cc:204`). See §4 for one open question on this.
- No post-processing stage. `RenderQueue::draw` renders straight to the default framebuffer.
- No mipmapping on any texture (`texture.cc:197` sets `GL_LINEAR`, no `glGenerateMipmap` anywhere).

**Engine constraints that shape everything below:**

- **OpenGL 2.1 / GLSL 1.20**, deliberately, for compatibility (`initialize.cc:44-45,260-261`; every
  shader opens with `#version 120`). No `flat` qualifier, no integer or bitwise ops in shaders, no
  `textureGrad`, no `#include`.
- **Terrain textures must be exactly 64x64**, enforced at load with an exception
  (`terrain_description.cc:242`, `kTextureSideLength` at `terrain_description.h:36`).
- **All terrain and road textures must fit in the *first* texture atlas**, asserted at startup
  (`build_texture_atlas.cc:113-159`). Both terrain programs assume a single GL texture.
- **Blits are axis-aligned only.** `BlitProgram` emits a quad from a `Rectf`
  (`blit_program.cc:124-148`); there is no rotation or shear.
- **No `.blend` files in this checkout** (zero hits). Upstream keeps media in a separate repository.

---

## 1. Why Widelands Terrain Looks Flat (Diagnosis — corrected)

The original diagnosis blamed flat per-triangle shading. That was wrong. The real cause is the
**texture repetition period**.

Terrain UVs are a world-space projection, not per-tile coordinates (`fields_to_draw.cc:145-148`).
Substituting the isometric constants `kTriangleWidth = 64`, `kTriangleHeight = 32`
(`ui/wui/mapviewpixelconstants.h:27-29`) and `kTextureSideLength = 64` gives the coordinate at a node:

```
u =  fx + 0.5 * (fy & 1)          v = -fy / 2
```

So the terrain texture repeats **once per field horizontally and once per two rows vertically** — a
64x64 pixel lattice on screen at zoom 1. The tiling is seamless (adjacent triangles of the same
terrain are continuous, because the UV is a continuous function of world position), but the period
is small enough that the eye reads it as wallpaper.

Two further notes:

- Height variation *is* shaded, per-vertex and smoothly. The original claim that shading only fires
  at slopes is half right for the wrong reason: on genuinely flat ground every node has the same
  normal, so brightness is uniform — but that is correct behaviour for a lighting model, not a bug.
  The missing ingredient is *material* variation, not *lighting* variation.
- The comparison to AoE holds up, but the reference images sharpen it. In
  `referenceImages/AoE2_0.png` most of the ground is *covered* by vegetation; the terrain texture
  barely matters because you can hardly see it. Ground-texture variation there is dominated by
  large-scale patches many tiles across, not by per-tile detail.

## 2. Terrain Fixes — Code-Side

Re-ranked after verification. Original numbering kept for cross-reference.

1. ~~**Smooth (Gouraud) shading**~~ — **already implemented.** See §0.

2. **Procedural low-frequency noise** (was: per-tile colour jitter) — **top priority, design worked
   out in `TERRAIN_NOISE.md`.** Corrected in one important respect: a per-*tile* constant jitter
   would produce a triangular patchwork, the same artefact at a different frequency. It has to be
   per-vertex (interpolated) or per-fragment. Decision: layered 2D simplex in the fragment shader,
   keyed on the existing `var_texture_position` varying. **Landed 2026-08-13**, then retuned after
   review: the first octave weighting produced regional tonal variation but left the 1-field repeat
   — the actual defect in §1 — untouched. The governing constraint is the antiphase rule
   (`TERRAIN_NOISE.md` §5): the octave that breaks a repeat of period *p* has wavelength 2*p*, so
   finer is worse, not better. See §14 for the review, §13 for the earlier (stale) numbers.
   Amplitude remains an open aesthetic call; phases 1b (tint axis), 2 (config toggle) and 3
   (per-terrain amplitude) are still open.

3. ~~**Randomize existing texture via rotation/mirroring**~~ — **rejected.** The UV field is
   continuous across triangle boundaries; rotating or mirroring per triangle would break that
   continuity and put a visible seam on every triangle edge. This works in AoE because AoE's tiles
   are discrete quads. Here it would look worse than what we have.

4. **Procedural noise overlay texture** — **superseded by #2.** A texture would need its own texture
   unit and a `GL_REPEAT` wrap mode (`Texture` sets `GL_CLAMP_TO_EDGE` unconditionally), cannot go in
   the atlas, and reintroduces a period — just a longer one. Procedural noise avoids all three.

5. **Procedural blend transitions** — **partly already there, and cheaper than assumed.**
   `DitherProgram` already does procedural transitions via a mask texture (`dither.fp:16-24`).
   Perturbing that mask lookup with noise makes every terrain border irregular at once, in a few
   lines. Note that whatever noise §2 applies must be applied identically in `dither.fp`, or every
   border gets a discontinuity — so these two are naturally one piece of work.

6. **Scatter layer** — **good idea, but not as written.** Placing actual immovables procedurally
   would change buildability, pathing, savegames and network sync; that is a gameplay change wearing
   a rendering costume. The version worth building is a **purely visual pass**: for each visible
   field, hash the coords and conditionally blit a small sprite at a hashed sub-field offset, keyed
   on terrain type, skipping fields that already carry an immovable or building. Nothing enters the
   map, nothing is saved, nothing can desync. `data/world/immovables/{plants,rocks,miscellaneous}`
   already supplies sprites, so the first version needs no new art. Given how much of the ground the
   reference images cover with clutter, this is probably the highest-payoff item after #2.

7. **Ambient occlusion in concave terrain** — viable, small. Implement it **render-side** in
   `FieldsToDraw::reset`, which already holds neighbour indices and heights — not in
   `Field::set_brightness`, which lives in `src/logic`, is an atomic recalculated on every height
   change, and would enlarge our footprint in the tree we most want to keep clean for rebases.

8. **Slow-moving cloud-shadow overlay** — unchanged, and cheap once #2 exists: it is the same noise
   function with a time-varying offset. Worth deferring until the static case looks right, since a
   moving multiply over the terrain is easy to overdo.

## 3. Terrain Fixes — Artist-Side, Ranked by Time Effort

Largely unchanged, with two corrections.

| # | Task | Effort | Notes |
|---|------|--------|-------|
| 1 | Single tileable noise/dirt overlay texture | Hours | **Superseded by §2.2** — procedural noise does this with no asset at all. |
| 2 | Recolored brightness/hue variants of existing tiles | Hours–1 day | Derivative edits, not new painting. Partly superseded by §2.2, which varies colour continuously rather than in discrete variants. |
| 3 | Small scatter/clutter sprite pack (rocks, tufts, twigs, cracks) | Few days | Still the best artist-time-to-payoff ratio, and §2.6 can start with existing world immovables before any new art is drawn. |
| 4 | Cliff/slope-specific textures | Several days–1 week | Constrained: every terrain texture must be exactly 64x64 and fit the first atlas. A slope-specific texture is a new terrain type, not a new render path. |
| 5 | Hand-painted blend/transition tiles | 1+ week | **Steer away.** `DitherProgram` already does this procedurally (§2.5). |
| 6 | New full tile variants | 1–2 weeks per terrain type | The 64x64 limit applies. There are 206 terrain PNGs across four worlds today. |
| 7 | Full high-res terrain retexture | Weeks–months | Would require lifting `kTextureSideLength` and re-checking the first-atlas budget. Structural, not just artistic. |
| 8 | Seasonal texture variants | Largest, open-ended | Unchanged: not worth starting until the base set is final. |

**Bottom line, revised:** items 1 and 2 are now code, not art. Item 3 remains the one place where
artist time buys the most, and even it can be deferred by reusing existing immovable sprites first.

## 4. Buildings

The structural point in the original stands and is confirmed: buildings are pre-rendered bitmaps,
so runtime per-pixel lighting is not available. Fixes lean on compositing around a fixed sprite.

**Open question to research:** shadows have been observed in gameplay video, which suggests some
sprites may carry baked shadows already. If so, item 1 below changes shape — casting a second,
engine-generated shadow on top of a baked one would look wrong, and the work becomes "make the baked
shadows consistent" rather than "add shadows". Worth settling before any code.

### Building fixes, by effort:payoff

1. **Cast drop shadows from the sprite's own alpha silhouette** — **confirmed absent, and closer to
   hand than assumed.** `BlitMode::kMonochrome` (`blit.fp:20`) computes `luminance * blend.rgb` with
   `alpha = blend.a * texture.a`; pass black with partial alpha and the result is exactly a flat
   silhouette at the sprite's own alpha, with no shader change. What is missing is (a) a monochrome
   path through `Animation::blit` → `MipMapEntry::blit` (two subclasses, small) and (b) shear, since
   blits are axis-aligned — adding a shear scalar to `BlitProgram::Arguments` and offsetting x by
   `shear * (y - bottom)` is the same six vertices computed differently. Apply it to trees and
   immovables too, not only buildings: in `referenceImages/AoE2_0.png` the tree shadows carry more
   of the effect than the building shadows do.
2. **Soft AO blob at the base footprint** — unchanged, layers with #1.
3. **Automated edge-detection fake-AO pass** — unchanged. Fully automatable preprocessing over
   existing sprites.
4. **Multiply-blend a shared grunge texture onto sprite bases** — weaker now that the terrain side is
   procedural; there is no longer a shared noise *asset* to reuse. Could sample the same procedural
   function, but sprites are blitted by a different program with different UVs, so it is not free.
5. **Per-instance random tint/brightness jitter** — needs a new blit flavour: flavour 0 ignores
   `blend.rgb` entirely and uses only `blend.a` (`blit.fp:17-18`). A modulate flavour is a few lines
   of GLSL plus plumbing. Low priority — Widelands' buildings are less visually repetitive than its
   workers.
6. ~~**Re-bake from source 3D models**~~ — **not possible in this checkout.** Zero `.blend` files;
   upstream keeps media in a separate repository. Would require pulling that in first.
7. **Rim/edge lighting on sprites** — unchanged, additive polish.

## 5. Broader Visual Fidelity Ideas

1. **AI upscaling on existing sprites** — **downgraded.** The mipmap system already exists:
   `ImageCache::kScales` = {0.5, 1, 2, 4} and `Animation::find_best_scale` (`animation.cc:182`) picks
   the smallest available scale ≥ the requested one, where `scale = 1/zoom`. At default zoom the
   engine uses the `_1` variant regardless of what else is on disk, so **upscaled art does nothing
   until the player zooms in**. Roughly 1811 assets already ship `_2` and `_4` variants (ships,
   workers, several production sites); buildings are thinner, about 460 of 3877 files. A real gap,
   but a data-side batch job with no code in it, and confined to zoomed-in play. Not the top of the
   list.
2. **Global color grading / LUT pass** — **not "one shader".** There is no post-process stage;
   `RenderQueue::draw` renders straight to the default framebuffer, and the UI goes through the same
   `BlitProgram` as the map. This needs a new offscreen FBO pass plus a decision about whether the UI
   gets graded too. `Texture` can already be an FBO target, so it is buildable, but it is plumbing in
   the hottest path in the renderer. Worth doing, after the harness can catch regressions.
3. **Animated water: scrolling normal map + specular** — premise corrected (water is already
   animated). Superseded by §6.
4. **Ambient life / particle effects** — unchanged, and still one of the larger contributors to
   "alive" in the references. No new sprite art needed.
5. **Soft fog-of-war edges** — **mostly already there.** See §0. The remaining win is perturbing the
   *outline* so it stops following the field grid; smaller than originally assumed.
6. **Day/night or dynamic sun-angle lighting** — the sun vector is a hard-coded static in
   `Field::set_brightness` (`field.cc:44-45`) and brightness is cached per field, recalculated only
   on height change (`map.cc:1438`). Animating it means recomputing whole-map brightness per step, or
   moving the lighting calculation render-side. Larger than it looks.
7. **Screen-space vignette + subtle bloom** — blocked on the same missing post-process stage as #2.
   Do them together or not at all.
8. **Higher-quality real-time scaling filter** (xBR/hqx) — same zoom-dependence caveat as #1.
9. ~~**Unit/worker animation smoothing**~~ — **already implemented.** See §0.

**Revised priority for this section:** #4 stands on its own. #2 and #7 are one piece of work behind
one piece of infrastructure. #1 and #8 are worth less than assumed. #3, #5, #9 are resolved.

## 6. Water: Depth-Based System

The mechanism is sound and the payoff is the most distinctive single thing in
`referenceImages/AoE_2.png` — a broad turquoise shallow zone fading to deep blue, with soft
irregular boundaries rather than bands. `TerrainDescription::Is::kWater` exists, and
`FieldsToDraw::Field` is the natural carrier for a per-node distance attribute.

Two findings the original did not account for:

- **The gradient is wide.** In the reference it spans roughly ten tiles, so a cheap local
  neighbour-count approximation will not produce it. The real BFS is needed, which means a cache and
  an invalidation story — the editor mutates terrain at runtime.
- **A depth field derived from true map terrain leaks information through fog of war.** Shallow-water
  shading next to an unexplored coastline tells the player land is there. The renderer already
  respects this elsewhere: `terrain_program.cc:111` uses `player->fields()[...].terrains` rather than
  the true terrain when the player cannot see all. The depth field has to do the same, which probably
  means computing it over *remembered* terrain per player rather than once per map. This belongs in
  the design before any code, because it changes where the cache lives.

The seven sub-effects (depth gradient, foam band, visible shallow bottom, caustics, low-frequency
noise layer, normal map + specular, cliff shadows onto water) are unchanged and still share the one
precompute. Note that #5 (low-frequency noise) is the same function §2.2 introduces, so that work
carries over.

Still the largest coding item here, and still almost no new art.

---

## Overall Priority Shortlist (revised)

1. **Terrain: layered procedural noise** (§2.2) — attacks the actual dominant defect; phase 1 is a
   shader-only change. Design in `TERRAIN_NOISE.md`. Includes matching `dither.fp` (§2.5).
2. **Render-only scatter/decoration layer** (§2.6) — biggest remaining gap against the references,
   pure code, can start with existing sprites.
3. **Drop shadows** (§4.1) — confirmed absent, machinery mostly present. Blocked on the baked-shadow
   question above.
4. **Water depth field** (§6) — biggest coding lift, highest payoff after the above, needs the fog-of-war
   question settled first.
5. **Concavity AO** (§2.7) — small, render-side, pairs naturally with #1.
6. **Post-process stage, then colour grading and vignette** (§5.2, §5.7) — one infrastructure change
   unlocking two effects. After the harness can catch regressions.

Dropped from the shortlist: Gouraud shading, per-tile rotation, the noise *texture*, AI upscaling,
animation smoothing, hand-painted transition tiles.

## Not on the original list

**Terrain has no mipmaps.** `texture.cc:197` sets `GL_LINEAR` and there is no `glGenerateMipmap`
anywhere, so at zoomed-out views 64x64 textures are minified unfiltered and shimmer under panning.
Fixing it is genuinely hard here: `terrain.fp:19-23` does its own `fract()` wrapping into a texture
atlas, so hardware mip selection would break at every wrap seam, and `textureGrad` is unavailable in
GLSL 1.20 without an extension. Recorded as a real defect, not proposed as work.
