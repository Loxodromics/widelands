# Renderer modernization: implementation plan

Cold-start document. It describes *what* work will be done, in *which order*, in units sized for
one working session each — not *how* each unit is implemented. It builds on, and supersedes as the
plan of record, the two other renderer documents:

- `RENDERER.md` — how the renderer works today (read this for the mechanics).
- `RENDERER_MODERNIZATION.md` — why modernize, and the argued option analysis behind this plan.

All paths are relative to the repository root.

---

## 1. Goal

Move Widelands' renderer from OpenGL 2.1 / GLSL 1.20 to a set of **switchable rendering backends**:

- **OpenGL 2.1** (GLSL 1.20) — kept permanently as the fallback for old machines.
- **OpenGL core profile** — the "modern OpenGL" backend (3.2 core on macOS, 3.3 core elsewhere,
  GLSL 150+).
- **Vulkan** — the future-facing backend; on macOS this runs through **MoltenVK** (Vulkan on
  Metal).

The game itself must behave identically on all three. Rendering is read-only on game state, so none
of this touches network determinism or replays.

A fourth target, **OpenGL ES 3.0** (GLSL 300 es), is treated as an optional future backend and is
kept in mind so the interface does not accidentally preclude it, but it is not part of this plan's
deliverables.

---

## 2. Current state (context)

- GL context is created through SDL2 (`src/graphic/gl/initialize.cc`) requesting 2.1; GLEW is the
  default loader, glbinding is an existing option (`src/graphic/gl/system_headers.h`).
- Eight shader programs (`data/shaders/*.{vp,fp}`, GLSL 1.20) cover everything drawn: blit, terrain,
  dither, road, workarea, grid, fill_rect, draw_line.
- Drawing is deferred: `RenderTarget`/`Surface`/`Image` collect draw calls into a `RenderQueue`
  (`src/graphic/render_queue.cc`), which sorts and batches by program/texture/blend and issues them
  in `Graphic::refresh()`.
- Each program owns one CPU vertex buffer uploaded to a VBO per frame (`Gl::Buffer`). There are no
  VAOs, no UBOs, no instancing.
- Textures: a texture atlas for terrain/roads (single-atlas invariant is enforced), standalone
  textures otherwise; `BlitData` = `{texture_id, parent_w, parent_h, subrect}`. The dither mask uses
  desktop-only `GL_INTENSITY`.
- One offscreen framebuffer (FBO) is used for render-to-texture. Screenshots use `glReadPixels`;
  texture readback uses `glGetTexImage`.
- A deterministic screenshot harness exists (`Claude/DEV_HARNESS.md`, `Claude/wl.py`): it can
  capture a known scene and assert **byte-identical** output via `--compare`. This is the regression
  gate for every work package below.

## 3. Locked-in decisions (from the research)

These are the parameters the plan is built on; changing one changes the ordering.

1. **Hand-roll a thin RHI**, do not adopt bgfx/Diligent/Magnum. The GL surface is small and stable.
2. **Single shader source + in-tree preprocessor** emitting GLSL 120, 150, and 300 es dialects; SPIR-V
   for Vulkan is generated later, only when the Vulkan backend lands (pre-compiled, not at runtime).
3. **Standardize on glbinding** as the only GL loader (drop GLEW); add **volk** for Vulkan.
4. **GL 2.1 stays as a permanent fallback backend.** The shader preprocessor must therefore keep
   emitting GLSL 120 for the life of the project.
5. **Explicit attribute locations** (`layout(location=N)`) everywhere, replacing `glGetAttribLocation`.
6. **No sRGB / filtering changes in the same change as any correctness work** — byte-identity from the
   harness is the acceptance gate, and those changes would break it legitimately.

---

## 4. Work-package format

Each work package (WP) is sized for one working session and lists:

- **Complexity** — Low, Medium, High, or Complex.
- **Depends on** — the WPs that must land first.
- **Goal** — the outcome, stated as a deliverable.
- **Scope** — what is in and (equally important) what is explicitly out.
- **Acceptance** — how to prove it done.

WPs are ordered in the recommended implementation sequence. Each leaves the tree building and
passing tests, so the project can stop after any WP.

Complexity scale, for calibration: **Low** = a focused, low-risk change in a couple of files;
**Medium** = a localized subsystem change with clear boundaries; **High** = a change that spans the
renderer and requires careful interface design; **Complex** = the genuinely hard, unfamiliar
territory (all of the Vulkan synchronization work).

---

## 5. The work packages

### Phase A — Foundations (cheap wins that everything else depends on)

**WP-1 — Standardize the GL loader on glbinding**
- Complexity: Low. Depends on: none.
- Goal: glbinding is the only GL loader; the GLEW path (`system_headers.h`, the `glewInit` block in
  `initialize.cc`) is removed.
- Scope: delete `OPTION_GLEW_STATIC` and the GLEW branch in CMake and `system_headers.h`; keep the
  `--debug_gl_trace` behavior working. No rendering changes.
- Acceptance: full build + ctest green with glbinding; `--debug_gl_trace` still works.

**WP-2 — Shader preprocessor with `#include`**
- Complexity: Medium. Depends on: none.
- Goal: a small in-tree shader preprocessor that (a) expands a single-level `#include` and (b) is
  designed to later emit multiple GLSL dialects; used now to factor the shared simplex-noise block
  into `data/shaders/noise.glsl` and remove the hand-copied duplicate in `dither.fp`.
- Scope: preprocessor + include expansion only. Dialect rewriting (120 vs 150 vs 300 es) is
  explicitly *not* done here — that is WP-5. This also closes backlog item 1b.
- Acceptance: terrain and dither still render byte-identically (`wl.py --compare`); noise block now
  lives in one place.

### Phase B — Modern OpenGL core backend

**WP-3 — Context/profile fallback chain**
- Complexity: Medium. Depends on: WP-1.
- Goal: context creation tries, in order: GL 3.3 core → GL 3.2 core (covers macOS, which has no 3.3)
  → GL 4.1 core (also macOS) → legacy GL 2.1 as the final fallback; the requested profile is
  recorded and exposed so later code can branch on it.
- Scope: the version/profiling request and the existing version-check machinery in `initialize.cc`;
  a small "which backend am I" value. No shader or draw changes.
- Acceptance: on a machine with only GL 2.1 the game still runs unchanged; on a modern driver it
  selects core profile and logs it.

**WP-4 — Vertex array objects**
- Complexity: Medium. Depends on: WP-3.
- Goal: every program owns a VAO that captures its attribute layout once; the per-frame
  `enable_vertex_attrib_array` bookkeeping in `Gl::State` is replaced by "bind the program's VAO".
- Scope: a small VAO wrapper and per-program setup. This is a prerequisite for core profile (which
  forbids VAO 0) and for explicit locations (WP-5).
- Acceptance: byte-identical output; `Gl::State` no longer tracks enabled attrib arrays per frame.

**WP-5 — Shader dialect migration (120 + 150) and explicit locations**
- Complexity: Medium. Depends on: WP-2, WP-4.
- Goal: shaders are written once in a core-style GLSL (150/300-es style) and the preprocessor emits
  both the GLSL 120 dialect (for the fallback) and the GLSL 150 dialect (for core); all attributes
  use explicit `layout(location=N)`.
- Scope: the eight shaders only. No change to draw semantics, uniforms, or C++ beyond switching to
  explicit locations (dropping `glGetAttribLocation`).
- Acceptance: byte-identical output on both the core and the 2.1 fallback path; shader source no
  longer contains `attribute`/`varying`/`gl_FragColor` in the *authored* file (the 120 output is
  generated).

**WP-6 — Texture format and readback correctness**
- Complexity: Medium. Depends on: WP-3.
- Goal: remove the two desktop-only/legacy texture behaviors: the dither mask's `GL_INTENSITY`
  upload becomes single-channel `GL_R8` (sampled as `.r`), and texture readback (`Texture::lock`)
  stops using `glGetTexImage` in favour of reading through the existing FBO, so core and future ES
  paths behave the same.
- Scope: texture upload and readback paths in `texture.cc`, the dither sampler, and the dither-mask
  load. No other programs.
- Acceptance: dither and terrain byte-identical; `Texture::lock`/screenshot paths correct on core.

**WP-7 — Depth precision**
- Complexity: Low. Depends on: WP-3.
- Goal: request a 24-bit depth buffer on core profile (2.1 stays at 16), removing the documented
  z-exhaustion artifacts.
- Scope: the depth-buffer request and the z-order comments; the 16-bit sort-key quantization in
  `render_queue.cc` is independent and stays.
- Acceptance: no visible z-fighting at high zoom on core; no behavior change on the 2.1 path.

**WP-8 — Uniform buffers (UBOs) for per-program state**
- Complexity: Medium. Depends on: WP-5.
- Goal: the per-program scalars (z-value, texture dimensions, and — once present — noise
  amplitudes) move from individual `glUniform*` calls into a small UBO per program.
- Scope: the uniform plumbing in the programs; this is the shape the Vulkan backend will need
  (descriptor-set/UBO style), so doing it here de-risks Phase D. It also lets backlog item 2 (noise
  amplitudes as uniforms) slot in.
- Acceptance: byte-identical output; `render_queue.cc` and the programs no longer call
  `glUniform*` per frame for these values.

At the end of Phase B the project has a working **modern OpenGL core backend** and a working
**OpenGL 2.1 fallback**, both drawing through the same eight programs. This is a shippable state on
its own.

### Phase C — The RHI seam

**WP-9 — RHI interface specification**
- Complexity: Medium. Depends on: Phase B (conceptually).
- Goal: a written interface (header + design notes, before implementation) for the backend
  abstraction, covering exactly what the eight programs and the four infra concerns need: texture
  (create/upload/subrect), buffer (create/upload), pipeline (program + vertex layout + blend +
  depth), bind/descriptor, pass/framebuffer (screen vs offscreen), and present.
- Scope: the *contract only*. It must be explicit about the leaks the research flagged: the
  single-atlas invariant for terrain/roads, and the GL [-1,1] vs Vulkan [0,1] depth-range mapping
  (the RHI owns that conversion, not the sort key).
- Acceptance: the interface is reviewed and agreed; every existing GL call site can be expressed in
  it without semantic loss (walk the eight programs against it).

**WP-10 — GL core backend behind the RHI**
- Complexity: High. Depends on: WP-9.
- Goal: the existing GL-core rendering code is re-expressed as one implementation of the RHI, with
  no visible change.
- Scope: move the program draw code and the state cache behind the interface. The `RenderQueue`
  batching logic stays exactly as it is (it is already backend-neutral). No Vulkan, no 2.1 changes.
- Acceptance: byte-identical output on core profile; the interface has a single, exercised
  implementation.

**WP-11 — GL 2.1 backend behind the RHI**
- Complexity: Medium. Depends on: WP-10.
- Goal: the GL 2.1 fallback becomes a second RHI implementation (or a shared GL implementation with
  a 2.1 mode), proving the interface is not secretly core-specific.
- Scope: the fallback path only. This is where the dialect split from WP-5 pays off: the 2.1
  backend consumes the GLSL 120 output.
- Acceptance: byte-identical output on both backends from one binary; the two GL backends share as
  much code as the interface allows.

At the end of Phase C the "switchable OpenGL2.1 / OpenGL3" half of the goal is real, and there is a
clean seam for Vulkan.

### Phase D — Vulkan backend (via MoltenVK on macOS)

**WP-12 — Vulkan bootstrap: instance, device, surface, swapchain**
- Complexity: High. Depends on: WP-9.
- Goal: create a Vulkan instance/device and a swapchain wired to the SDL window; on macOS this is
  MoltenVK. `volk` is the loader.
- Scope: init/teardown and surface/swapchain acquisition only — no drawing yet. Includes MoltenVK
  packaging/loading on macOS and the runtime detection that picks Vulkan when available.
- Acceptance: the game opens a window and presents an empty clear colour through Vulkan (MoltenVK
  on macOS, native on Linux/Windows).

**WP-13 — Vulkan pipelines and render pass**
- Complexity: Complex. Depends on: WP-12.
- Goal: the eight programs become a pre-built set of `VkPipeline` objects (program × blend mode ×
  render pass), created up front and cached.
- Scope: render pass setup and pipeline creation. No descriptor sets, no vertex upload yet.
- Acceptance: the pipeline cache builds for all eight programs and both blend modes without errors;
  pipeline layout matches the RHI's vertex layouts.

**WP-14 — Vulkan buffers, vertex upload, draw recording**
- Complexity: High. Depends on: WP-13.
- Goal: per-frame vertex data is staged and uploaded, and the `RenderQueue`'s batching is recorded
  as `vkCmdDraw` calls with the right pipelines.
- Scope: buffer/staging management and command recording. No per-texture binding yet (WP-15), no
  multi-frame synchronization yet (WP-16).
- Acceptance: a single-frame, single-texture render path (e.g. one atlas bound) draws correctly.

**WP-15 — Vulkan descriptor sets and texture upload**
- Complexity: Complex. Depends on: WP-14.
- Goal: texture binding (the role `Gl::State::bind` plays today) becomes descriptor-set management,
  preserving the atlas subrect semantics of `BlitData`.
- Scope: descriptor set layouts/pools, texture upload, and per-draw binding; explicitly handles the
  single-atlas invariant for terrain/roads.
- Acceptance: the full scene renders correctly with texture binding; byte-identical to the GL
  backends (modulo any documented, agreed output differences — there should be none).

**WP-16 — Frames in flight, synchronization, present**
- Complexity: Complex. Depends on: WP-15.
- Goal: proper multi-frame pipelining — 2–3 frames in flight, fences/semaphores for
  acquire/submit/present, and ownership of the depth-range mapping (GL [-1,1] → Vulkan [0,1]).
- Scope: the part with no GL analogue. Command recording stays on the UI thread (no render thread
  in this plan).
- Acceptance: steady-state 30 FPS without validation errors, no stalls, correct frame ordering.

**WP-17 — Vulkan readback**
- Complexity: Medium. Depends on: WP-16.
- Goal: screenshots and `Texture::lock` readback through host-visible buffers instead of
  `glReadPixels`/`glGetTexImage`.
- Scope: the two readback paths only.
- Acceptance: screenshots via the harness are byte-identical across all three backends.

### Phase E — Consolidation

**WP-18 — Runtime backend selection and cleanup**
- Complexity: Low. Depends on: Phase D.
- Goal: backend is chosen at startup (command line/config, with an automatic fallback: Vulkan →
  GL-core → GL 2.1), a clear error is shown when a chosen backend is unavailable, and dead code
  from the transition is removed.
- Scope: selection logic, user-facing error handling, documentation updates (`RENDERER.md`,
  this plan's status), and deletion of any now-unused code.
- Acceptance: all three backends are selectable and fall back sensibly; the suite and harness are
  green on each.

---

## 6. Ordering rationale

1. **Phase A first** because WP-1 (glbinding) and WP-2 (preprocessor) are cheap, low-risk, and are
   prerequisites for later phases; WP-2 also closes an already-agreed backlog item.
2. **Phase B before the RHI** because it de-risks the design: the RHI is easier to specify against a
   core-profile renderer with VAOs, explicit locations, and UBOs already in place. Every WP in
   Phase B ends byte-identical, so regressions are caught immediately.
3. **The RHI seam (Phase C) before Vulkan** is the crux of the whole plan: it forces the
   backend-independent interface to exist and be exercised by *two* GL implementations before the
   hard Vulkan work starts. WP-9 (spec) deliberately precedes any code.
4. **Vulkan (Phase D) last and strictly ordered**: each WP builds a vertical slice (bootstrap →
   pipelines → buffers/draw → descriptors → sync). WP-16 (sync/frames-in-flight) is the only
   genuinely open-ended piece and is isolated so it can be worked on without destabilizing the rest.
5. **Phase E** is a deliberate cooling-down step that makes the result discoverable and removes the
   scaffolding.

## 7. Milestones (states worth stopping at)

- **M1** — end of Phase B: modern GL-core backend + GL 2.1 fallback, shippable.
- **M2** — end of Phase C: switchable GL 2.1 / GL-core behind one RHI; the seam exists.
- **M3** — end of Phase D: Vulkan (incl. MoltenVK on macOS) complete and byte-identical.
- **M4** — end of Phase E: user-selectable backends, cleaned up, documented.

## 8. Risks and notes

- **Byte-identity is the standing gate.** Any WP that legitimately changes pixels (there should be
  none in this plan) must be called out and agreed before it is accepted.
- **The single-atlas invariant** (terrain/roads in one texture) must be carried into the RHI
  contract and the Vulkan descriptor design, or it will silently break terrain batching.
- **Depth range is backend-specific** (GL [-1,1], Vulkan [0,1]); the RHI owns this, not
  `render_queue.cc`.
- **Vulkan sync (WP-16) is the only part with no GL analogue** and is the most likely to need a
  second session; it is isolated for that reason.
- **Threading model does not change**: command recording stays on the UI thread; rendering stays
  read-only on game state, so lockstep determinism and replays are unaffected throughout.
- **GLES 3.0** is intentionally out of scope but the dialect and readback choices in Phase B keep
  the door open for a later ES/WebGL backend without rework.

## 9. Where to go next

- `RENDERER_MODERNIZATION.md` — the full argument for every decision in section 3.
- `RENDERER.md` — the current architecture; WP authors should read the relevant sections before
  each package.
- `Claude/DEV_HARNESS.md` — how to run the byte-identity regression gate (`wl.py --compare`) used in
  every acceptance criterion.
- `Claude/backlog.md` — the terrain-noise phases; WP-2 and WP-8 fold in items 1b and 2 respectively.
