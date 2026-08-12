# Code review: Widelands renderer

Review of `src/graphic/`, `src/ui/wui/mapview*`, `src/graphic/gl/`.
Reviewed for correctness, architecture, and modern C++ usage, plus robustness,
performance, portability, and testability.

## Verdict

A pragmatic, well-aged deferred-rendering architecture with several genuine
latent bugs, mostly in rarely-exercised paths (validation, atlas packing, cache
edge cases). The GL abstraction and the RenderQueue design are the strongest
parts; the animation validation and the workarea program are the weakest. The
code is conservative C++11/14 in a C++17 project - readable, but the
`RenderQueue::Item` layout and the hand-rolled caches are the places a modern
rewrite would pay off.

## 1. Correctness

### C1 - Dead validation: spritesheet playercolor check compares a value to itself. (real bug, low severity)

`src/graphic/animation/spritesheet_animation.cc:206-213`:

```cpp
const bool should_have_playercolor = first.has_playercolor_masks;
for (const auto& mipmap : mipmaps_) {
    if (first.has_playercolor_masks != should_have_playercolor) {  // always false
```

The condition never uses the loop variable, so mixed playercolor-mask
availability across scales loads silently and only fails later at blit time.
The equivalent check in `nonpacked_animation.cc:205` is written correctly -
this is a copy-paste slip.

### C2 - Texture atlas can hang the startup if a block cannot fit. (latent bug, low likelihood)

`src/graphic/texture_atlas.cc:168-171`: `pack()` loops `while (!blocks_.empty())`;
a block that never fits (side + padding > `max_dimension`, e.g. a 2048x28 PNG
passes the <=240x240 *area* filter in `build_texture_atlas.cc:37` but is 2049 px
wide) is permanently re-queued into `not_packed` -> infinite loop at startup.
There is no guard. Worth one `if (can_grow_* == false) throw wexception(...)`.

### C3 - `FieldsToDraw::reset` writes out of bounds on the capped-resize path. (latent bug)

`src/graphic/gl/fields_to_draw.cc:114-126`: if `dimension > max_dimension`, the
vector is resized to `max_dimension` but `w_`/`h_` stay at full range; the fill
loops at lines 128-172 index `fields_[calculate_index(fx, fy)]` past the end -
`std::vector::operator[]` UB. The log message tells the user to restart, but
the code does not actually degrade gracefully. Also `w_ * h_` is computed as
`int32_t` before the `size_t` cast (line 115) - signed overflow if it ever got
near 2^31.

### C4 - BlitProgram batching predicate is asymmetric. (latent bug, no visible effect today)

`src/graphic/gl/blit_program.cc:92-95`: the batch-merge condition breaks only
when the *current* item has a mask differing from the template's. A mask-less
item (`mask.texture_id == 0`) following a mask-bearing template is silently
merged into the batch, and its mask rect is computed via
`to_gl_texture(BlitData{0, 0, 0, Rectf()})` -> division by zero
(`parent_width == 0`, `coordinate_conversion.h:64-68`) -> NaN/Inf varyings. The
per-vertex `program_flavor` masks it in the shader (`blit.fp:17-27`), so output
is currently correct - but every direct blit in the game feeds a NaN mask
varying to the GPU, and the batching state (bound mask texture) is wrong in
principle. The predicate should also compare `blit_mode`, or the mask rect
computation should guard zero dimensions.

### C5 - Z-layer exhaustion is handled inconsistently between debug and release. (documented, unfixed)

`src/graphic/render_queue.cc:52-76`: `make_key_*` `assert(z_value < 65535)`
crashes debug builds; `next_z_` then overflows and `to_opengl_z` (line 39-41)
produces out-of-NDC z values in release. The TODO at line 214 references the
known bug (launchpad #1658593). The depth buffer is also 16-bit
(`initialize.cc:46`) while z is quantized to 16 bits - ties are resolved by the
sort, and opaque ties with `GL_LEQUAL` can layer wrong. This is acknowledged
debt, but the z budget is now also silently consumed by UI text re-rendering,
so it is worth re-surfacing.

### C6 - `TextureAtlas` padding comment is wrong. (nit)

`texture_atlas.cc:29-32` claims padding is applied "to the left and bottom",
but the split at line 42-43 places it on the right and bottom. Harmless, but
the comment misleads the next person tuning atlas packing.

### C7 - `SpriteSheetAnimation::blit` assert off by one. (nit)

`spritesheet_animation.cc:109`: `assert(idx <= columns * rows)` should be `<`.
Unreachable in practice because `nr_frames_ <= rows*columns` is validated at
load.

### C8 - `WorkareaProgram` border-color index can underflow. (latent, invariant-guarded)

`src/graphic/gl/workarea_program.cc:251-294`: with more than three border rings
per workarea entry, `index` becomes -1 and `workarea_colors[-1]` is UB.
Currently impossible (`get_workarea_overlay` throws for radii > 3,
`interactive_base.cc:685-701`), and the assert catches debug builds, but the
array indexing should be `at()`-style guarded.

### C9 - `calculate_line_normal` NaNs on zero-length segments. (nit)

`src/graphic/surface.cc:41-47`: a duplicate point in a line strip yields 0/0 ->
NaN vertices. All current callers pass sane geometry; a cheap guard (`len == 0`
-> skip segment) would make it robust.

### Minor findings

- `glDrawArrays(GL_TRIANGLES, 0, vertices_.size())` passes `size_t` to
  `GLsizei` without a cast in six programs.
- `AnimationManager::get_representative_image` (`animation_manager.cc:67`) keys
  its cache by `RGBColor*` pointer identity rather than color value (harmless
  today because callers pass stable player colors).
- `ImageCache::get` (`image_cache.cc:130`) does `hash.rfind('.')` unguarded and
  throws `std::out_of_range` on extension-less filenames.

## 2. Architecture

### Strengths

- **Clean layering**: `RenderTarget` (clip/offset windows) -> `Surface`
  (abstract dest) -> `Screen` (enqueue) / `Texture` (immediate FBO rendering).
  The dual execution path is documented in `render_queue.h:68-70` and respected
  everywhere.
- **Deferred queue + batching** is genuinely good: per-vertex z and
  `program_flavor` allow merging items with different depths into one
  `glDrawArrays`, and the opaque front-to-back + depth test pass avoids
  overdraw (`render_queue.cc:231-239`).
- **The single-atlas invariant** for terrain/roads is enforced by throwing in
  `build_texture_atlas.cc:155-158` instead of asserting - good.
- `ScopedScissor`, `GlFramebuffer`, `Gl::State` caching: RAII and state-diffing
  done right.

### Weaknesses

- **`RenderQueue::Item` "logical union"** (`render_queue.h:129-160`): every
  enqueue copies all four argument structs, including the big ones
  (`TerrainArguments.workareas`, `LineArguments.vertices`).
  `Screen::do_draw_line_strip` even moves into the item and then copies again.
  Worse, `Workareas` is deep-copied three times per frame
  (`get_workarea_overlays` returns a copy, `terrain_arguments.workareas =
  workarea` copies, the enqueue copies). A C++17 `std::variant` would both
  shrink the item (~max member instead of sum) and make the "which member is
  live" invariant self-checking - the comment's cache-friendliness argument
  points the wrong way.
- **Scene caching lives inside a GL program**: `WorkareaProgram`'s per-view
  cache (`workarea_program.cc:180-188`) is a scene concern. It works, but only
  because the hit test compares topleft fcoords + pixel + full workarea
  equality - and the `<=`/`>=` double comparison (lines 181-185) is just `==`
  written awkwardly. The cache is invalidated implicitly on every one-pixel
  pan. This belongs in `MapView`/`InteractiveBase`, not in a program singleton.
- **Program singletons with mutable vertex buffers** - fine single-threaded,
  but it means the programs are not reentrant and the GL state machine must
  never be touched from the logic thread. Currently true; nothing enforces it
  (only the `is_initializer_thread()` asserts in `Gl::State`).
- **Error handling stops at init**: `glGetError` is never polled in the frame
  loop (only via the optional glbinding trace). Pragmatic for a game, but it
  means C2-C4 class bugs surface as corruption, not as errors.

## 3. Modern C++ (C++17 in a C++11-style codebase)

The codebase is uniformly conservative; the renderer is no exception.
Reasonable uses: `unique_ptr`, `move`, `emplace_back`, `constexpr`,
`[[nodiscard]]`, Meyers singletons, RAII scopes. Gaps worth fixing when the
surrounding code is touched:

- `std::make_unique` is essentially unused (`new Texture(...)` wrapped in
  `unique_ptr` in at least ten places in `graphic/` alone).
- `enum Program`/`enum UnlockMode` are unscoped enums stored as bare `int`
  (`Item::program_id`) - `enum class` would remove the `kHighestProgramId <= 8`
  static-assert dance and the default-throw branches.
- `Animation::kSupportedScales` is a `std::map<float, std::string>` with a
  reverse-comparator map `mipmaps_` to get reverse iteration over four entries
  (`animation.h:168-175`); a `std::array` iterated backwards is simpler and
  allocation-free. (`find_best_scale` itself is correct - the reverse iteration
  against `MipMapCompare` was verified.)
- `Shader::compile`/`Program::build` allocate `char[]` manually for info logs
  (`gl/utils.cc:119-129`); `std::string` is available.
- `ImageCache::images_`/`mipmap_cache_` are `std::map` where `unordered_map` is
  the natural fit for the hot `get()` path.
- The `RGBAColor(0, 0, 0, 255 * opacity)` pattern (`screen.cc:61`) truncates
  float->`uint8_t`; fine, but `std::lround` would avoid the 1/255 rounding
  artifacts.
- `WorkareaProgram::add_vertex` writes `offset.x > 0 || offset.x < 0 || ...`
  for "non-zero" (`workarea_program.cc:113`) - `offset != Vector2f::zero()`
  says it better.

## 4. Own review criteria

### Robustness / UB hygiene

The renderer is deterministic, reads-only on game state, and network-sync safe -
drawing never mutates simulation state (sound triggers via notifications are the
only side channel, correctly non-simulation). The UB risks are C2, C3, C8 - all
on invariant-dependent paths.

### Performance

The two sorts per frame and the per-program single VBO uploads are sound. The
real per-frame cost driver is the unconditional full redraw (documented design,
30 FPS cap). The three-item deep copy chain for `Workareas` (see above) is the
only accidental cost. `Texture::do_blit*` execute immediately mid-frame and
flush GL state (`glFlush` on every FBO switch, `utils.cc:229-232` - Intel
workaround), which is a hidden cost whenever minimap/playercolor textures are
drawn between queued screen items.

### Portability

GL 2.1/GLSL 1.20 is a deliberate and well-documented choice, but `GL_INTENSITY`
(`texture.cc:140`, used by the dither mask) is desktop-only - the code already
anticipates GLES2 in comments (`texture.cc:112-116`), so this is a
known-but-unmarked landmine for any GLES port. On macOS, the SDL2/GL 2.1 path
is fine today, but the deprecated-CGL future (SDL3/EGL) would touch
`initialize.cc`.

### Testability

There is no unit test coverage for the renderer beyond the richtext renderer
test (`src/graphic/text/test/`). The clip math in
`RenderTarget::clip`/`to_surface_geometry`/`enter_window` and the sort-key
logic in `render_queue.cc` are pure functions and would test well without a GL
context; the coordinate-conversion functions in `coordinate_conversion.h` as
well. This is the cheapest high-value test investment available.

## 5. Prioritized fix list

1. C2 - guard `TextureAtlas::pack` against un-fittable blocks (startup hang).
2. C3 - cap the *drawn range*, not the vector, in `FieldsToDraw::reset` (OOB write).
3. C1 - fix the spritesheet playercolor validation copy-paste.
4. C4 - make the blit batch predicate symmetric / guard `to_gl_texture` on zero-size `BlitData`.
5. Eliminate the `Workareas` triple-copy (move instead of copy, or store a pointer in `TerrainArguments`); move `Item` in `enqueue`.
6. Consider `std::variant` for `Item` and `enum class` for `Program` - both are localized, low-risk modernizations with real size/type-safety payoff.
7. Add pure-math unit tests for `RenderTarget` clipping and `RenderQueue` sort keys.

## Bottom line

No findings that break gameplay or determinism. The strongest pieces (deferred
batching, atlas enforcement, state caching) are also the ones carrying the most
comments - which is the right balance for a 20-year-old codebase.
