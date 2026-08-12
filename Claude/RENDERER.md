# Widelands Renderer Architecture

This document describes how the renderer works in Widelands (C++17, SDL2, OpenGL 2.1).
All paths are relative to the repository root.

## Overview

| Class | File | Responsibility |
|---|---|---|
| `Graphic` (`g_gr`) | `src/graphic/graphic.h`, `graphic.cc` | Owner of the graphics system: SDL window + GL context, `Screen`, `RenderTarget`, resolution management, texture-atlas rebuild, screenshots, `refresh()`. Singleton global `g_gr`. |
| `RenderTarget` | `src/graphic/rendertarget.h`, `.cc` | The drawing API used by all UI/game code. Wraps a `Surface` with a clip rect + offset. All primitives go through here: `blit`, `blitrect_scale`, `tile`, `fill_rect`, `draw_line_strip`, `blit_animation`. |
| `Surface` | `src/graphic/surface.h`, `.cc` | Abstract destination (`blit`, `blit_blended`, `blit_monochrome`, `fill_rect`, `draw_line_strip`). Converts pixel rects to GL renderbuffer coordinates. |
| `Screen` | `src/graphic/screen.h`, `.cc` | The concrete `Surface` representing the window. Its `do_*` methods only enqueue items into the `RenderQueue`. Also `to_texture()` reads the framebuffer back (screenshots). |
| `Texture` | `src/graphic/texture.h`, `.cc` | A GL texture that is also a `Surface` (render target via FBO) and an `Image` (blit source). Offscreen rendering (minimap, playercolor images) renders into a `Texture`; draw calls targeting a texture execute immediately, only screen draws are deferred. |
| `RenderQueue` | `src/graphic/render_queue.h`, `.cc` | Singleton, deferred rendering. Screen draw calls are enqueued as `Item`s, sorted, batched by program, and executed in `Graphic::refresh()`. |
| `Image` | `src/graphic/image.h` | Interface for blit sources; `blit_data()` returns the `BlitData` (GL texture id + subrect). |

## Frame flow (main loop to screen)

```
main()  (src/main.cc)
  └─ WLApplication::run()  (src/wlapplication.cc:945)
       └─ UI::Panel::do_run()  (src/ui/basic/panel.cc:361)   [main loop, 30 FPS cap]
            │  while running_:
            │    handle_input()
            │    if (start_time >= next_time):
            │       do_think()        (panel.cc:435)  -- UI think(), locks kObjects
            │       do_redraw_now()   (panel.cc:442)
            │    SDL_Delay(...); next_time += kDrawDelay (= 1000/30)
```

`do_redraw_now()` (panel.cc:267):
1. `rt = *g_gr->get_render_target()` — screen `RenderTarget`, reset to full window.
2. The panel tree draws itself (`Panel::do_draw`, panel.cc:1205): each panel calls `dst.enter_window(...)` for its rect, draws border, then children back-to-front.
3. Mouse cursor + tooltips drawn.
4. `g_gr->refresh()` (graphic.cc:354):
   - `RenderQueue::instance().draw(w, h)` (render_queue.cc:214) — the *only* place issuing draw calls to the screen: bind default framebuffer, `glViewport`, `glClear`, enable depth test; opaque items drawn front-to-back with blending disabled, blended items back-to-front with `GL_BLEND`.
   - screenshot handling if requested.
   - `SDL_GL_SwapWindow()` (graphic.cc:383).

A separate logic thread (`Panel::logic_thread`, panel.cc:203) runs at its own cadence
(`kGameLogicDelay = 50ms`, panel.cc:200) calling `game_logic_think()` → `Game::think()`
(src/logic/game.cc:872). It synchronizes with the UI thread via a mutex; game logic and
rendering are decoupled. Map animations run in *real time* (`SDL_GetTicks()`), not game
time (mapview.h:90-92).

## OpenGL abstraction

- `src/graphic/gl/system_headers.h` is the *only* place GL headers are included. Uses GLEW
  by default, or glbinding if built with `USE_GLBINDING` (CMake option
  `OPTION_USE_GLBINDING`).
- Targets **OpenGL 2.1 / GLSL 1.20** deliberately for compatibility (initialize.cc:44-45,
  260-261; shaders in `data/shaders/*.vp`/`.fp` use `#version 120`).
- `Gl::initialize()` (gl/initialize.cc:34): creates the GL context, initializes GLEW,
  checks GL >= 2.1 / GLSL >= 1.20, queries max texture size, `SDL_GL_SetSwapInterval(0)`.
- `Gl::State` (gl/utils.cc:179): caches GL driver state (texture bindings, framebuffer,
  vertex attrib arrays) to skip redundant GL calls. Calls `glFlush()` on framebuffer
  switches (Intel driver bug), and unbinds textures before using them as FBO attachments.
- `Gl::Program`/`Shader` (gl/utils.cc:81): compiles shaders from `data/shaders/`, links
  programs; throws `wexception` with the info log on failure.
- `Gl::Buffer<T>` (gl/utils.h:62): VBO wrapper; `update()` always re-uploads via
  `glBufferData(GL_DYNAMIC_DRAW)` because partial updates stall drivers.
- Optional `--debug_gl_trace` logs every GL call (only with glbinding).
- Programs (one per shader, in `src/graphic/gl/`): `blit_program`, `terrain_program`,
  `dither_program`, `road_program`, `workarea_program`, `grid_program`,
  `fill_rect_program`, `draw_line_program`. Each accumulates vertices in a CPU vector,
  uploads once per frame, then issues `glDrawArrays(GL_TRIANGLES)`.
- `coordinate_conversion.h` converts pixel coordinates to GL space (renderbuffer coords in
  [-1,1] with y-flip, texture coords in [0,1]).

## Texture / image management

- Loading: `image_io.cc` decodes PNG/JPEG with SDL_image; `Texture(SDL_Surface*)`
  (texture.cc:109) converts BGR→RGBA, flips rows, uploads with `glTexImage2D`.
  Parameters: `GL_LINEAR` filter, `GL_CLAMP_TO_EDGE`. No hardware mipmaps.
- "Mipmaps" are pre-rendered scale variants: `ImageCache::kScales` = {0.5→`_0.5`, 1→`_1`,
  2→`_2`, 4→`_4`} (image_cache.h:38). The best scale for the current zoom is selected at
  blit time.
- `ImageCache` (`g_image_cache`): permanent hash→Image map, lazy loading on the
  initializer thread via `NoteThreadSafeFunction`. Owns all images for program lifetime.
- `playercolor_image()` (graphic/playercolor.cc:30) caches per-color blended images
  (filename + `+pc` + hexcolor). Player-color masking uses `*_pc.png` mask images blended
  with `blit_blended`.
- Texture atlas: built once at startup by `Graphic::rebuild_texture_atlas()`
  (graphic.cc:150), called from `WLApplication::initialize()`. Packs all images under
  `world/terrains`, `tribes/initialization`, `images/` (≤ 240×240 px) into big GL
  textures (`build_texture_atlas.cc`, `texture_atlas.cc`, binary-tree packing with 1 px
  padding). Terrain and road textures are *required* to land in the first atlas
  (asserted) because the terrain/road programs assume a single GL texture.
  `BlitData` = `{texture_id, parent_width/height, rect}` so blits work transparently
  from atlas or standalone textures.

## Isometric map view

- Constants: `kTriangleWidth = 64`, `kTriangleHeight = 32`, `kHeightFactor = 5`
  (src/ui/wui/mapviewpixelconstants.h:27-29).
- `MapView` (src/ui/wui/mapview.cc): panel showing the map; owns `View` (viewpoint +
  zoom, range 1/4 .. 4) and animation plans for pan/zoom. `draw_terrain()`
  (mapview.cc:368).
- `FieldsToDraw` (src/graphic/gl/fields_to_draw.cc:72): computes which fields are visible
  (with margins for triangle boundaries and height), and per-vertex GL data: position
  (via `MapviewPixelFunctions::map_to_panel` minus `height * kHeightFactor`), texture
  coords (pseudo-random tiling: `map_pixel / kTextureSideLength`), brightness, neighbor
  indices, visibility. A slope-occlusion pass marks fields hidden behind taller terrain.
- Fog of war: `InteractivePlayer::draw_map_view` overrides per-field brightness, roads,
  seeing state, owner from `player->fields()`; the terrain program uses the player's
  terrain data when not fully visible (terrain_program.cc:111).
- Terrain pipeline (`Graphic::game_renderer.cc` / `draw_terrain`, game_renderer.cc:53):
  enqueues `kTerrainBase` (opaque), `kTerrainDitherOrHeightHeatMap`, optional
  `kTerrainWorkarea`, `kTerrainGrid`, `kTerrainRoad`.
  - TerrainProgram: each field up to 2 triangles (right/down), 3 vertices each; texture
    offset is the atlas origin of the terrain's current texture (terrains can be
    animated: `TerrainDescription::get_texture(gametime)`).
  - DitherProgram: terrain-transition dither triangles between differing dither layers.
  - RoadProgram: a quad per road segment between field centers.
  - WorkareaProgram: translucent overlay rings; GridProgram: grid lines; FillRectProgram:
    also height heat-map overlays.
- Map objects drawn after terrain per visible field (interactive_player.cc:544-587):
  `Immovable::draw`, `Building::draw`, `Bob::draw`, `Worker::draw` — all call
  `dst->blit_animation(...)` with the field pixel position, game-time offset, zoom scale,
  and player color. Construction sites draw frame-by-frame from build progress with
  `percent_from_bottom` cropping.

## RenderQueue (performance core)

- Every screen draw is an `Item` with program_id, z_value, a 64-bit sort key, blend mode,
  and union-style arguments (render_queue.h:129).
- Z-ordering: monotonically increasing `next_z_`, converted to GL depth in [-1,1].
- Sort keys: opaque `(program << 60) | (extra << 16) | (maxZ - z)` — program-major,
  front-to-back so no pixel is drawn twice; blended `(z << 40) | (program << 36) | extra`
  — back-to-front for correct alpha. `extra` for blits is the texture id, enabling
  batching by texture.
- Batching: consecutive same-program items are merged; BlitProgram batches items sharing
  texture, mask, and blend mode into one `glDrawArrays` (blit_program.cc:86). This
  reduces GL call count by an order of magnitude.

## Animations

- Defined in data via Lua in each tribe object's `init.lua`:
  - Spritesheet (preferred): `spritesheets = { idle = { hotspot = {22, 69}, frames = 10,
    columns = 5, rows = 2, fps = 10 } }` (e.g.
    `data/tribes/buildings/militarysites/frisians/wooden_tower/init.lua:33`).
  - File-per-frame (older): `animations = { idle = { basename = "none", hotspot = {7, 32}
    } }` referencing `idle_1_00.png` style files.
  - Optional keys: `fps` (default 250 ms/frame, animation.h:36), `play_once`,
    `sound_effect`, `directional` (6 directions), `representative_frame`.
- Loading: Lua description → `add_animations()` (src/logic/map_objects/map_object.cc:210)
  → `AnimationManager::load()` (animation_manager.cc:31) constructs
  `NonPackedAnimation` or `SpriteSheetAnimation`. Scale variants (`_0.5`/`_1`/`_2`/`_4`)
  are registered as mipmap entries; scale 1.0 is mandatory. Graphics load lazily on first
  blit; `*_pc.png` masks are auto-detected.
- Rendering: `RenderTarget::blit_animation` (rendertarget.cc:307):
  `Animation::current_frame(time) = (time / frametime) % nr_frames`, clamped at last
  frame if `play_once`. Source rect may be cropped (`percent_from_bottom`, construction
  sites); destination is aligned by hotspot at the zoom scale with the best available
  mipmap scale. `_pc` masks blit with player color via `blit_blended`.
  `trigger_sound()` publishes a `NoteSound` when frame 0 plays (ambient sounds).
- Representative images (menus/help) generated on demand and cached per (animation id,
  player color) in `AnimationManager` (animation_manager.cc:66).

## Fonts / text

- `FontHandler` (`UI::g_fh`, font_handler.cc:54): `render(text, maxwidth)` →
  `RenderedText`; results cached in a `RenderCache` (TransientCache counting rects),
  glyph bitmaps in a byte-budget LRU `TextureCache` (3 MB). Rendering happens on the
  initializer thread.
- `RT::Renderer` (text/rt_render.cc): parses rich-text markup (`<rt>`, fonts, colors,
  images, tables, tooltips) into `RenderedRect`s (text/rendered_text.h).
- `FontSet` per locale/script parsed from `i18n/fonts.lua`; SDL_ttf rasterizes glyphs,
  uploaded as textures; shadows drawn with 1px offset.
- `Graphic::max_texture_size_for_font_rendering()` clamps to 2048 in debug builds.

## Performance mechanisms

1. Deferred rendering + sorting/batching in the RenderQueue (one vertex-buffer upload per
   program per frame).
2. Opaque front-to-back rendering with depth test (pixels drawn once).
3. Texture atlases for terrain/road/flag/UI images (fewer texture binds).
4. `Gl::State` caching of binds/framebuffers/attrib arrays.
5. View culling in `FieldsToDraw`; torus-aware distance checks in
   `MapView::ViewArea::contains` (mapview.cc:289).
6. LRU transient caches for text/glyphs; permanent ImageCache; cached mipmap-bitmaps.
7. Incremental minimap rendering (a slice of map rows per frame).
8. Lazy animation loading on first use.
9. Zoom-dependent asset selection (image "mipmaps").
10. Double buffering, vsync off, 30 FPS UI cap; game logic on a separate thread.
11. `Gl::Buffer::update` always re-allocates with `GL_DYNAMIC_DRAW` (avoids
    command-buffer stalls from partial updates).

There is no general per-widget dirty-flag system — the whole UI tree redraws each 30 FPS
tick. Dirty tracking exists only in specific places (minimap rows, `set_force_draw` on
scrollbars).

## Key file references

- Main loop: `src/ui/basic/panel.cc:361` (do_run), `:267` (do_redraw_now), `:203` (logic
  thread); `src/wlapplication.cc:945` (run), `:1163` (handle_input)
- Frame end: `src/graphic/graphic.cc:354` (refresh → RenderQueue::draw →
  SDL_GL_SwapWindow)
- GL init: `src/graphic/gl/initialize.cc:34`; headers: `src/graphic/gl/system_headers.h`;
  state cache: `src/graphic/gl/utils.cc:179`
- RenderQueue: `src/graphic/render_queue.h:78`, `.cc:214` (draw), `:52` (opaque key),
  `:68` (blend key)
- Screen: `src/graphic/screen.cc:54`; Texture/FBO: `src/graphic/texture.cc:109`
- Terrain: `src/graphic/gl/fields_to_draw.cc:72`, `terrain_program.cc:86`; map view:
  `src/ui/wui/mapview.cc:368`; game renderer: `src/graphic/game_renderer.cc:53`
- Atlas: `src/graphic/build_texture_atlas.cc:106`, `texture_atlas.cc:154`; ImageCache:
  `src/graphic/image_cache.cc:99`
- Animations: `src/graphic/animation/animation.cc:35`, `:113`; `spritesheet_animation.cc`;
  `nonpacked_animation.cc`; manager `animation_manager.cc:31`; data:
  `data/tribes/buildings/.../init.lua`
- Text: `src/graphic/font_handler.cc:82`, `text/rt_render.cc`, `text/rendered_text.h:127`
- Isometric constants: `src/ui/wui/mapviewpixelconstants.h:27`
