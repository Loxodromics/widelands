# Renderer modernization: research and options

Cold-start document. Read this before `RENDERER.md` (how the renderer works today) and
`RENDERER_CODE_REVIEW.md` (what is wrong with it). This document answers the question: *how could
we move the renderer to modern OpenGL (GL3+) or Vulkan, ideally with a switchable backend, and what
would we actually be committing to?*

All paths are relative to the repository root. Facts about the current code were verified against
`src/graphic/` and `data/shaders/` at the time of writing.

---

## 1. Summary / recommendation

The renderer's GL surface is already narrow: eight shader programs, a per-program vertex buffer, a
state cache, textures, one offscreen framebuffer, and a screen swapchain. Everything else talks to
`RenderTarget`/`Surface`/`Image`, not to GL. That is a good seam to build a backend behind.

Recommended path, in order:

1. **Retarget to GL 3.2 core + GLES 3.0** (a single "core profile" GL backend), not GL 3.3. This
   is a small, mechanical change that buys most of the practical benefits (modern GLSL, VAOs, UBOs,
   no `GL_INTENSITY` landmines, ES portability) at low risk, and it is the necessary stepping stone
   for anything else.
2. **Introduce a thin render-hardware interface (RHI)** behind the existing eight programs, then
   back it with a second implementation (Vulkan via MoltenVK). Do not adopt a full RHI library;
   hand-roll a minimal one.
3. **Leave OpenGL 2.1 as a fallback backend for as long as it is cheap.** Because the seam is small,
   keeping three backends (GL2.1 / GL-core / Vulkan) behind one interface is realistic — that is the
   "switchable backend" the request asks for.

Skip a full Vulkan-only rewrite; it buys little for a batched 2D renderer and costs the most.
The pragmatic ordering is: **GL-core first, RHI seam second, Vulkan third, keep GL2.1 as a fallback.**

---

## 2. Why modernize at all

The current renderer (OpenGL 2.1 / GLSL 1.20, GLEW or glbinding) works, and
`RENDERER_CODE_REVIEW.md` found no gameplay-breaking bugs. The arguments for modernizing are
strategic, not urgent:

1. **macOS.** Apple deprecated OpenGL in 10.14 and never shipped GL 4.2+. On Apple Silicon, OpenGL
   runs on top of Metal. GL 2.1 (the legacy/compatibility profile) still works today but is the
   most deprecated of all options. This is the strongest single forcing function: the Mac build is
   stranded on an API the platform owner wants gone.
2. **GLSL 1.20 is a dead end.** `attribute`/`varying`, `texture2D()`, `gl_FragColor`, no
   `#include`, no integer/bit ops, no UBOs. The terrain-noise work already hit this: `dither.fp`
   carries a hand-copied duplicate of `terrain.fp`'s noise block precisely because GLSL 1.20 has no
   include mechanism (see the TODO in `data/shaders/dither.fp` and backlog item 1b).
3. **Core-profile hygiene.** The code uses desktop-only features that block any GLES/WebGL port and
   are removed in core profile: `GL_INTENSITY` (the dither mask, `texture.cc:140`), and a reliance
   on the default vertex-array object (VAO 0, which is invalid in core profile).
4. **Vulkan as a strategic option.** A Vulkan backend removes the GL loader/context dance entirely,
   gives explicit control over memory and synchronization, and is the only forward-looking path on
   macOS (via MoltenVK) and mobile. It is not needed for correctness, but "switchable backend"
   implies having more than GL.
5. **Driver bugs.** `Gl::State::bind_framebuffer` already carries an Intel-specific `glFlush()`
   workaround (`gl/utils.cc:229`). GL 2.1 drivers are where the most crutches accumulate; newer
   profiles and Vulkan move those crutches out of our code.

Non-argument: **performance.** The renderer is 30 FPS-capped and already deferred/batched; a
backend change is not expected to move the needle measurably for a 2D game. Modernization here is
about portability and future-proofing, not speed.

---

## 3. What "switchable backend" actually means here

The GL surface that a backend must cover is confined to `src/graphic/` and `data/shaders/`. A
concrete inventory of the touchpoints:

| Concern | Today | Files |
|---|---|---|
| Context / window / swapchain | `SDL_GL_*` in `Graphic`, `Gl::initialize` | `graphic.cc:86,128,169,383`, `gl/initialize.cc:34` |
| GL loader | GLEW or glbinding | `gl/system_headers.h` |
| State cache | `Gl::State` (textures, FBO, attrib arrays) | `gl/utils.cc:179` |
| Programs / shaders | 8 programs, one per `data/shaders/*.{vp,fp}` | `gl/*_program.cc`, `gl/utils.cc:146` |
| Vertex buffers | `Gl::Buffer<T>` (VBO, no VAO) | `gl/utils.h:62` |
| Textures / atlas | `Texture`, `BlitData` (`texture_id` + subrect) | `texture.cc`, `gl/blit_data.h` |
| Offscreen target | single `GlFramebuffer` + FBO texture | `texture.cc:66` |
| Frame draw + clear + present | `RenderQueue::draw` + `Graphic::refresh` | `render_queue.cc:214`, `graphic.cc:354` |
| Readback | `glReadPixels` (screenshot), `glGetTexImage` (lock) | `screen.cc:41`, `texture.cc:220` |

The eight programs and their vertex layouts:

| Program | Attributes | Uniforms | Draw |
|---|---|---|---|
| `blit` | pos(3), tex(2), mask-tex(2), blend(4), flavor(1) | `u_texture`, `u_mask` | batched quads |
| `terrain` | pos(2), tex-pos(2), tex-offset(2), brightness(1) | `u_terrain_texture`, `u_texture_dimensions`, `u_z_value` | field triangles |
| `dither` | pos(2), tex-pos(2), dither-tex(2), tex-offset(2), brightness(1) | `u_dither_texture`, `u_terrain_texture`, `u_texture_dimensions`, `u_z_value` | dither triangles |
| `road` | pos(2), tex(2), brightness(1) | `u_texture`, `u_z_value` | quads |
| `workarea` | pos(2), overlay(4) | `u_z_value` | translucent tris |
| `grid` | pos(2), color(3) | `u_z_value` | lines |
| `fill_rect` | pos(3), color(4) | — | rects |
| `draw_line` | pos(3), color(4) | — | line strips |

Key observation: the renderer is **already fully shader-based** — no `glBegin/glEnd`, no matrix
stack, no fixed-function pipeline. "Modernizing" is therefore *not* the classic fixed-function→core
rewrite. It is: core-profile compliance, GLSL version migration, VAO/UBO introduction, and — if we
want a second backend — lifting these eight programs plus the four infra concerns behind an RHI.

Two facts that make this cheaper than it looks:

- The whole game already draws through `RenderTarget`/`Surface`. Only `Screen` (deferred) and
  `Texture` (immediate) touch GL, and both funnel into the same eight programs.
- The `PerVertexData` structs in each program are already clean interleaved vertex layouts; mapping
  them to Vulkan vertex input bindings is mechanical, not a redesign.

---

## 4. The four design decisions that matter

### 4.1 Which GL target

The tempting answer is "GL 3.3 core", but that is the *one* core version macOS does not expose.
macOS ships exactly two core profiles: **3.2 core** and **4.1 core** (GLSL 150 / 410). Linux and
Windows ship everything.

Consequences:

- A "GL 3.3 core" target silently becomes "4.1 core on Mac, 3.3 core elsewhere", or fails to
  request the right profile and falls back to legacy 2.1 on Mac. Either is a trap. The honest
  baseline is **GL 3.2 core** (GLSL 150), which runs everywhere desktop GL exists, with a couple of
  features gated behind extensions if we want them (`GL_ARB_explicit_attrib_location`,
  `GL_ARB_texture_storage`, `GL_ARB_framebuffer_sRGB`).
- GLES is the natural second baseline: **GLES 3.0** (GLSL 300 es) is the smallest common
  denominator that matches GL 3.2 core closely enough to share shader logic, and it unlocks
  ANGLE/WebGL/Mobile later. The code already anticipates ES in comments (`texture.cc:112`,
  `blit_data` uses explicit parent width/height because ES lacks NPOT guarantees).
- Decide *early* whether GL2.1 stays as a fallback. If yes, we commit to a shader-source strategy
  that can emit GLSL 120 *and* 150/300-es (section 4.3). If no, we can drop GLSL 120 entirely and
  simplify. Recommendation: keep it; the incremental cost is one extra shader flavour and a
  backend flag, and it preserves a working path while the new backends mature.

### 4.2 Hand-rolled RHI vs. adopting a library

This is the highest-leverage decision. The candidates:

| Option | What it is | Cost | Fit |
|---|---|---|---|
| **Hand-rolled RHI** | A `Renderer` interface: `create_texture/buffer/pipeline`, `begin/end_pass`, `bind`, `draw`, `present`. Two impls: GL-core, then Vulkan. | Medium (well-understood, contained to `src/graphic/gl` + a new `src/graphic/rhi` or `renderer/`) | **Best.** The GL surface is small and stable; we already own equivalent abstractions (`Gl::Program`, `Gl::Buffer`, `Gl::State`). |
| **bgfx** | Cross-API (GL/GLES/Vulkan/D3D/Metal) renderer with its own shader compiler. | Medium-high (new dependency, its own build + shader toolchain, forced fit into its command-encoder model) | Poor. Its abstractions (views, encoders) don't map to our RenderQueue cleanly, and it drags in shaderc. |
| **Diligent Engine** | Low-level RHI (GL/Vulkan/D3D/Metal). | High (C++ heavy, large API surface, still a big dependency) | Overkill for eight programs. |
| **Magnum** | Full graphics wrapper library. | High (effectively adopting a framework) | Overkill; it would want to own more than just rendering. |
| **SDL3 `SDL_gpu`** | SDL's own cross-backend GPU API (D3D12/Metal/Vulkan). | Medium, but bundled with a full SDL2→SDL3 migration | Intriguing long-term, not a first step. |

Rationale for hand-rolling: the project historically vendors only small libraries (eris, gettext,
libmd, minizip) and has deliberately kept GL behind a thin, self-written layer for two decades. The
eight programs + four infra concerns are a *small* interface. An external RHI would fight the
existing `RenderQueue` batching design and the `Image`/`Texture` split more than it would help. The
risk of hand-rolling is that we under-engineer the Vulkan side (descriptor sets, pipeline cache,
synchronization), so the interface must be specified up front with those in mind (section 5.2),
not grown organically from the GL code.

### 4.3 Shader source strategy

GLSL 120 → core/ES/Vulkan means picking how shaders are written and how they become each backend's
shading language. Options:

1. **N shader files, one per dialect.** `blit.vp` becomes `blit_120.frag`, `blit_150.frag`,
   `blit_300es.frag`, plus SPIR-V for Vulkan. Blunt but transparent; four copies of trivial
   shaders is a maintenance drag and invites the `terrain.fp`/`dither.fp` drift again.
2. **Single source + textual preprocessor.** Write one GLSL (say, 300 es / 330 core style) and use a
   small in-tree preprocessor to emit the 120 dialect (rewrite `in/out`→`attribute/varying`,
   `texture()`→`texture2D()`, inject `#version`) and to do the `#include` expansion we already want
   for the noise block (backlog 1b). This is the pragmatic sweet spot, and it reuses the
   `Program::build` include-expansion idea.
3. **SPIR-V as the single source + SPIRV-Cross** to emit GLSL 120/150/300-es/Vulkan SPIR-V. This is
   the "proper" answer and is what real engines do, but it pulls in glslang + SPIRV-Cross (sizeable
   build deps, and not currently vendored) for eight shaders whose total complexity is low.

Recommendation: **option 2 now** (single GLSL source + preprocessor, emitting 120 and
150/300-es), and only adopt glslang/SPIRV-Cross (option 3) *if* we add a Vulkan backend, where
SPIR-V is mandatory. Even then, the eight shaders can be pre-compiled to SPIR-V at build time and
committed, avoiding a runtime compiler dependency. Do not maintain per-dialect copies (option 1) —
we already have evidence (`dither.fp`) that duplicated shader code drifts.

Also worth deciding now: **explicit attribute locations** (`layout(location=N)`). This is available
in 330/300-es via extension in 150, and is what makes the GL and Vulkan backends agree on vertex
layouts. Since the current code queries `glGetAttribLocation` by name (`blit_program.cc:45`), moving
to explicit locations removes that query and makes the layouts declarative — a prerequisite for a
shared RHI.

### 4.4 GL loader: GLEW vs glbinding vs glad

Currently both GLEW (default) and glbinding (`OPTION_USE_GLBINDING`) are supported
(`system_headers.h`, `CMakeLists.txt:142`). For a core-profile + Vulkan world:

- **GLEW** is the thing `system_headers.h` already calls "really a crappy piece of software" and the
  comments long to drop it (`system_headers.h:27-34`). Its 2.1-era error workarounds (Wayland, GLEW
  error 4) are noise we can delete.
- **glbinding** is already wired up and supports GL 4.6 cleanly; standardizing on it is the
  lowest-friction move, and it keeps the `--debug_gl_trace` feature working.
- **glad** is a lighter alternative, but it means replacing an existing integration for no clear
  gain.

Recommendation: **make glbinding the only loader** (delete the GLEW path) as part of the GL-core
work. For Vulkan, add **volk** (a tiny meta-loader) — the established minimal choice. This is a
dependency-reduction step, not a new risk.

---

## 5. The two big pieces of work

### 5.1 GL-core retarget (the "modern OpenGL" ask)

Mechanical, bounded, testable with the existing dev harness (`Claude/DEV_HARNESS.md`, `wl.py
--compare` for byte-identical captures). The concrete changes:

1. **Context request.** `initialize.cc:44-45` requests 2.1. Change to request 3.2 core
   (`SDL_GL_CONTEXT_PROFILE_MASK = SDL_GL_CONTEXT_PROFILE_CORE`) — with a fallback path that tries
   core first and, if unavailable, requests 4.1 core (Mac), then legacy 2.1 as last resort. The
   version-check machinery (`initialize.cc:197`) already exists and just needs the thresholds moved.
2. **VAOs.** Core profile forbids drawing with VAO 0. Each program needs a VAO created and bound
   once (a small `Gl::VertexArray` RAII wrapper), wrapping the existing `glVertexAttribPointer`
   setup. The per-frame `enable_vertex_attrib_array` bookkeeping in `Gl::State` can then shrink —
   the layout becomes fixed per program.
3. **Shader migration** (via the preprocessor, section 4.3): `attribute/varying`→`in/out`,
   `texture2D`→`texture`, `gl_FragColor`→declared `out`, `#version 120`→`#version 150`
   (or 300 es). The eight shaders are short; this is the same change applied eight times.
4. **`GL_INTENSITY` removal.** The dither mask (`texture.cc:140`) becomes a single-channel `GL_R8`
   texture; the sampler reads `.r` instead of relying on intensity broadcast. This also unblocks
   GLES.
5. **Readback paths.** `glGetTexImage` (`texture.cc:220`) is desktop-only (not in ES); switch
   `Texture::lock()` to read through the existing FBO + `glReadPixels` so both desktop-core and ES
   behave the same. `screen.cc:41` is already `glReadPixels` — fine.
6. **Depth precision.** Currently 16-bit (`initialize.cc:46`) with `GL_LEQUAL`. Core profile allows
   24-bit; keep the 16-bit *sort-key quantization* in `render_queue.cc` (it's independent) but
   request a 24-bit buffer to remove the documented z-exhaustion artifacts (`render_queue.cc:214`
   TODO, `RENDERER_CODE_REVIEW.md` C5).
7. **Uniforms.** Optionally move the per-program scalars (`u_z_value`, `u_texture_dimensions`,
   `scale`, noise amplitudes) into a single UBO per program. Not required for core, but it is the
   shape the Vulkan backend needs, so doing it here halves the later work. Backlog item 2 (noise
   amplitudes as uniforms) slots in here naturally.

Risk: low. The dev harness's `--compare` gives byte-identity checks for the whole surface. The one
place byte-identity can legitimately drift is if we also fix the gamma/sRGB handling — so **do not
enable sRGB in the same change** (section 6).

### 5.2 RHI + Vulkan backend (the "Vulkan" ask)

Only after 5.1. The RHI interface, specified up front, covers exactly what the eight programs and
four infra concerns need:

- `Texture` (create, upload, sub-rect region via the existing `BlitData` semantics),
- `Buffer` (create, upload — the whole-buffer re-upload model in `Gl::Buffer::update` maps to
  Vulkan's per-frame ring/staging buffers),
- `Pipeline` (program + vertex layout + blend mode + depth mode; the current per-program
  `DrawBatch`/blend logic maps to Vulkan `VkPipeline` + color-blend state),
- `DescriptorSet`/bind (texture units — this replaces `Gl::State::bind`),
- `Pass`/framebuffer (screen vs. the offscreen FBO; the single-FBO invariant stays),
- `Present` (swapchain acquire/submit).

The genuinely new Vulkan complexity, and what the interface must not paper over:

- **Pipeline objects.** GL compiles/link lazily and tolerates state changes; Vulkan pipelines are
  immutable and must exist for each (program × blend × renderpass) combination. With eight programs
  and two blend modes (`Copy`, `UseAlpha`) that is a small, pre-buildable set — a pipeline cache keyed
  by the same sort-key fields `RenderQueue` already has.
- **Descriptor sets.** GL's "bind texture 0 and 1" becomes descriptor set updates. The `Gl::State`
  caching of "is texture X already bound" maps naturally onto descriptor reuse; per-frame updates of
  a few sets is fine at this scale.
- **Synchronization and frames in flight.** This is the part with no GL analogue: swapchain image
  availability, `VkFence`/`VkSemaphore` per frame in flight, and where the per-frame vertex upload
  buffers live. It is bounded (2–3 frames in flight, one render pass) but it is real work and the
  main reason the Vulkan backend should not be attempted *before* the RHI exists.
- **Depth range.** GL maps z to [-1,1]; Vulkan maps to [0,1]. The `to_opengl_z`
  (`render_queue.cc:39`) conversion is backend-specific; the RHI must own that mapping rather than
  baking GL's convention into the sort key. This is exactly the kind of leak the RHI is for.
- **Readback.** `Screen::to_texture`/`Texture::lock` become buffer copies in Vulkan (read from a
  host-visible buffer rather than `glReadPixels`).

The RenderQueue batching logic itself is backend-neutral and stays as-is: it already sorts by
(program, texture, blend) and issues offset draws, which is precisely Vulkan's `vkCmdDraw` with
different descriptors.

---

## 6. Options, argued

### Option A — GL 3.2/4.1 core only (no RHI, no Vulkan)
Modernizes GL, keeps a single backend. Lowest risk, immediate macOS and GLSL relief, ES-ready. Does
not satisfy "switchable backend". **Best first milestone**, not an end state.

### Option B — RHI over GL-core + Vulkan, keep GL2.1 as fallback *(recommended end state)*
One interface, three implementations. Satisfies "switch OpenGL2.1 / OpenGL3 / Vulkan" literally.
Highest effort, but the effort is front-loaded into interface design and the Vulkan plumbing; the GL
backends are cheap because they reuse the existing program code nearly verbatim. The fallback GL2.1
backend means we can land the RHI incrementally and always have a working build.

### Option C — adopt bgfx/Diligent/Magnum
Gets "all backends" for free in theory; in practice it imports a large dependency, its own shader
toolchain, and forces our RenderQueue/Image/Texture design to bend around someone else's
command-encoder model. The project's dependency history and the small size of our GL surface argue
against it. Reject unless there is a future need (e.g. D3D/Metal backends) we don't have.

### Option D — Vulkan only (drop GL)
Cleans out GL entirely and is the most future-proof on macOS/mobile, but it throws away a working,
well-understood GL path to jump straight into the hardest part (Vulkan sync/frames-in-flight) with
no intermediate milestone to verify correctness. Reject as a *first* step; revisit only if GL
support actually rots to the point of being unmaintainable.

### Option E — SDL3 + `SDL_gpu`
SDL3's `SDL_gpu` gives D3D12/Metal/Vulkan behind one API, but it bundles a full SDL2→SDL3 migration
(upstream Widelands is on SDL2) and the API is young. Not a near-term option; note it in case the
project migrates to SDL3 for other reasons.

---

## 7. Risks, caveats, open questions

- **Pixel-identity vs. correctness.** Modernizing can silently change output: core-profile texture
  filtering/rounding, 24-bit depth, and especially sRGB are all "improvements" that will break the
  byte-identity the dev harness currently asserts. Keep sRGB and any filtering change *out* of the
  GL-core milestone; the harness's `--compare` then stays a meaningful regression gate.
- **Threading.** The current code is deliberately single-UI-thread + initializer-thread
  (`is_initializer_thread()` asserts in `Gl::State`). Vulkan's explicit submission model *invites*
  a dedicated render thread, but that is a separate, risky change and is not required for lockstep
  determinism (rendering never mutates simulation state). Keep command recording on the UI thread
  initially.
- **Determinism is not touched.** Rendering reads game state and never writes it; no backend change
  affects network sync or replays. This is the single biggest risk that does *not* exist, and it is
  worth stating explicitly for a cold-start reader.
- **The atlas invariant.** Terrain/road require a single texture atlas (asserted in
  `build_texture_atlas.cc:155`). A backend must preserve "one texture per terrain atlas" or relax
  the program to array-of-textures. This is unchanged by GL-core but interacts with Vulkan
  descriptor binding — flag it in the RHI design.
- **Effort estimate.** GL-core retarget is on the order of days for one person familiar with the
  code, including the shader preprocessor. The RHI + Vulkan backend is on the order of weeks to
  months, dominated by Vulkan synchronization and pipeline/descriptor plumbing, not by the eight
  shaders.
- **Open questions to settle before starting the RHI:**
  - Do we keep GL2.1 as a permanent fallback, or drop it once GL-core and Vulkan are proven?
  - Pre-compile shaders to SPIR-V and commit them, or require a shader compiler at build time?
  - Do we adopt `layout(location=)` everywhere (recommended) and stop using `glGetAttribLocation`?
  - Is GLES 3.0 (and later ANGLE/WebGL) an actual target, or just a nice-to-have that should not
    constrain the interface?

---

## 8. What to read next

- `RENDERER.md` — the current architecture in detail (frame flow, RenderQueue, atlas, programs).
- `RENDERER_CODE_REVIEW.md` — correctness and modernization findings; several (C5 z-exhaustion, the
  `std::variant`/`enum class` modernizations, the `#include` expansion) are directly relevant here.
- `Claude/DEV_HARNESS.md` — how to get deterministic, diffable screenshots; the regression gate for
  any renderer change.
- `Claude/backlog.md` §3 — terrain-noise phases; phase 1b (shader `#include`) is the natural first
  slice of the shader-preprocessor work in section 4.3.
