# Dev harness

How to drive Widelands from a shell command for development work — past the menus, into a known
scene, with a screenshot and a readable log out the other end. Written primarily so an agent can
iterate on the renderer without a human clicking through menus, but it is useful by hand too.

Tracked items live in `backlog.md` section 1. This document is the design and the reference.

## The goal

One command → a deterministic, diffable screenshot of a known scene, plus a log we can grep.
"Deterministic" is the hard part and is discussed under [Determinism](#determinism).

---

## What already exists

Established by reading the tree. Recorded here so we don't re-derive it or rebuild it.

### Command line

The CLI is considerably richer than the `--help` summary suggests. Full table in
`src/wlapplication_messages.cc:60-290`; parsing in `src/wlapplication.cc:1645`
(`handle_commandline_parameters`).

| Option | Use for us |
|---|---|
| `--scenario=FILE` | Start a map directly as a singleplayer scenario |
| `--loadgame=FILE\|last` | Load a savegame directly, or the most recent one |
| `--replay=FILE\|last` | Load a replay directly |
| `--editor[=FILE\|last]` | Straight into the map editor |
| **`--script=FILE`** | **Run a Lua script after init.** Valid with `--scenario`, `--loadgame`, `--editor` |
| **`--homedir=DIR`** | **Isolated config/savegame/screenshot profile** |
| `--datadir=DIR` | Data files, if running a binary outside the source tree |
| `--display_flags=<bitmask>` | Map overlays — see below |
| `--xres=N` `--yres=N` | Fixed window size. Small and fixed = fast and stable captures |
| `--nosound` / `--play_intro_music=false` | Skip audio and the splash screen |
| `--messagebox-timeout=<s>` | Auto-close modal dialogs so a run can't hang on one |
| `--fail-on-lua-error` / `--fail-on-errors` | Turn silent breakage into a non-zero exit |
| `--verbose` | Enables the `verb_log_*` family |
| `--enable_development_testing_tools` | Script console and cheat mode |

Use `--homedir` on **every** harness run. It keeps our runs from touching the real config,
savegames and screenshot folder.

`--display_flags` is a bitmask over the `dfShow*` enum in `src/ui/wui/interactive_base.h:75-92`:
census, statistics, soldier levels, workarea overlap, debug, buildings, buildhelp, maximum
buildhelp, grid, immovables, bobs, resources, oceans, height heat map. Default is
`dfShowSoldierLevels | dfShowBuildings | dfShowWorkareaOverlap` (`interactive_base.h:91`).

Note on the mouse cursor: `--sdl_cursor` defaults to true, meaning the *system* draws the cursor, so
it does not appear in a framebuffer readback. Leave it alone — setting it false would draw the
cursor into our screenshots.

### Lua

`--script=FILE` is the main lever. It is a scripting hook into a fully loaded game, which covers
camera positioning, overlay toggling and game control without any new CLI surface.

`wl.ui.MapView()` (`src/scripting/ui/lua_map_view.cc:45-79`) currently offers:

- Methods: `click`, `scroll_to_field`, `scroll_to_map_pixel`, `mouse_to_field`, `mouse_to_pixel`,
  `start_road_building`, `abort_road_building`, `close`, `is_visible`, `add_toolbar_plugin`,
  `set_keyboard_shortcut`, plugin timers, and `subscribe_to_*` callbacks.
- Properties: `average_fps` (RO), `center_map_pixel` (RO), `is_animating` (RO), `is_building_road`
  (RO), `toolbar` (RO), and read/write `buildhelp`, `census`, `statistics`.

`wl.ui` module functions (`src/scripting/lua_ui.cc:386`) include `set_user_input_allowed`,
`show_messagebox`, clipboard access and shortcut queries.

The established script shape is a coroutine — see `test/scripting/load_and_quit.lua`:

```lua
include("scripting/coroutine.lua")

run(function()
  sleep(3000)
  wl.ui.MapView():close()
end)
```

Note that `sleep()` here is **game time**, not real time: the coroutine is resumed by
`CmdLuaCoroutine` on the game's command queue (`data/scripting/coroutine.lua:58`), and at speed 0
the queue never advances, so a coroutine can never wake up. This is why the capture sequence below
is C++-driven instead of a Lua script.

### Logging

`do_log()` (`src/base/log.cc:175`) formats `[HH:MM:SS.mmm real|game] LEVEL: message` and writes to
stdout via the SDL logger (`log.cc:112`) on everything except Windows, which gets a `stdout.txt`.
So **logging to a file already works today** via `> log.txt 2>&1`. What is missing is filtering and
subsystem tags, not the file itself.

Levels are `kInfo`, `kDebug`, `kLua`, `kWarning`, `kError`, with `log_*` and `verb_log_*` macro
families (`src/base/log.h:44-78`); the `verb_` ones are gated on `--verbose`.

### Screenshots

`Graphic::screenshot(fname)` (`src/graphic/graphic.cc:389`) sets `screenshot_filename_`; the PNG is
actually written at the end of the *next* `Graphic::refresh()` (`graphic.cc:376-381`) via
`screen_->to_texture()` and `save_to_png`. It is currently reachable only from a keyboard shortcut
(`src/wlapplication.cc:1140`).

### Regression harness

`regression_test.py` already runs maps with Lua scripts against a built binary, with a temp homedir
and parallel workers. `test/maps/*.wmf`, `test/scripting/*.lua`. It is the working model for what we
are building, and worth reading before writing `wl.py`.

---

## Deterministic capture (Milestone 1)

The first milestone is done: `Claude/wl.py` drives the binary to a deterministic, diffable
screenshot of a known scene. Everything needed lives in the new `src/dev_harness/` module plus a
few small call-outs in upstream files.

### New command line switches

| Option | Meaning |
|---|---|
| `--capture=FILENAME` | Enable capture mode. The PNG is written to `screenshots/FILENAME` **inside the home directory** — see path note below |
| `--capture-at=<ms>` | Gametime at which to freeze and capture (default 0). A **lower bound**, not an exact time — see the note below |
| `--capture-view=<x>,<y>,<zoom>` | Map-pixel coordinates of the top-left corner of the view and the zoom, applied as an instant jump |
| `--capture-show-ui` | Keep the toolbar and info panel. They are hidden by default because they break reproducibility — see [Residual nondeterminism](#residual-nondeterminism) |
| `--fixed-timestep=<ms>` | Advance gametime by a fixed amount per logic tick; capture mode defaults it to 50 |

Valid with `--scenario`, `--loadgame`, `--editor` (like `--script`); everything else is rejected at
parse time, as is `--capture-at` > 0 with `--editor`. Note on the editor: it has no game controller,
so the simulation cannot be frozen by one — but its gametime *does* advance, by wall clock in
`EditorInteractive::think()`. That drives the frame animation of water and immovables, which would
make editor captures non-reproducible, so capture mode pins the editor's gametime instead (same
mechanism as the overlay cleanup: gated on capture mode, `editorinteractive.cc`). The sub-switches
require `--capture`.

### `--capture-at` is a lower bound

The freeze happens at the first logic tick whose gametime is at or after the requested value, and
loading a map advances gametime in bulk before the first tick is observed. The overshoot can
therefore be much larger than one tick. Measured on `test/maps/plain.wmf`:

| Requested | Captured |
|---|---|
| 0 | 0 |
| 1000 | 2550 |
| 30000 | 30050 |

Each value is reproducible across runs, so this costs precision, not determinism. The gametime
actually captured is written to the log (`Dev harness: capturing ... at gametime N ms`) — treat that
line, not the requested value, as the truth about what a capture shows.

### Why the animation phase is reproducible

Three findings decided the design (they correct earlier guesses in this document):

1. **Map animations are gametime-driven, not real-time**: `blit_animation(..., Time(gametime -
   animstart_), ...)` at `src/logic/map_objects/bob.cc:824` and `immovable.cc:434`. Only camera
   panning uses `SDL_GetTicks()` (`src/ui/wui/mapview.cc:374`) plus minor chrome (minimap attack
   blink, debug FPS text). So **pausing the game does freeze the map**.
2. **Gametime advances by a real-time-derived amount**: `CmdQueue::run_queue` computes
   `final_time = game_time_var + interval` *before* its loop (`src/commands/cmd_queue.cc:98`), and
   the interval comes from `SinglePlayerGameController::think()` differencing `SDL_GetTicks()`
   (`src/logic/single_player_game_controller.cc:50`). The gametime a run reaches therefore depends
   on machine speed and frame rate — upstream acknowledges this at `src/logic/game.cc:884`. Without
   fixing this, the animation phase differs between runs no matter when we pause.
   `--fixed-timestep` replaces the wall-clock delta with a constant, so the logic thread (running at
   `kGameLogicDelay = 50 ms`, `src/ui/basic/panel.cc:200`) advances the game by exactly that amount
   per tick. It is off by default, so nothing changes for normal play.
3. **The capture sequence cannot live in Lua**: `sleep()` yields an absolute gametime
   (`data/scripting/coroutine.lua:58`) and resumes via `CmdLuaCoroutine` on the game queue. With
   speed 0 the coroutine never wakes, so a script cannot pause, capture, then quit. Sequencing is
   C++-driven:
   - `SinglePlayerGameController::think()` freezes the simulation (`set_desired_speed(0)`) at the
     first logic tick at or after `--capture-at`. Because ticks advance by a fixed amount, the
     freeze gametime is exactly reproducible and independent of frame timing. Freezing on the logic
     thread also means the command queue has fully processed the previous tick when the UI observes
     the freeze.
   - `DevHarness::Capture::think()` (ticked from `InteractiveBase::think()`, UI thread) waits for
     the freeze, applies the view and hides the chrome, lets the scene settle for a few frames,
     requests the screenshot, polls `Graphic::screenshot_pending()`, then closes the game with
     `end_modal(kBack)` — the same mechanism Lua's `MapView:close()` uses
     (`src/scripting/ui/lua_map_view.cc:311`). Under `--loadgame`/`--scenario` this exits the
     process: `WLApplication::run()` falls through to `should_die_ = true`
     (`src/wlapplication.cc:1058`).

### Screenshot path note

`Graphic::screenshot()` writes through the layered filesystem (`g_fs->open_stream_write`,
`graphic.cc:378`), which roots **every** path — including absolute ones — inside its first
writable layer, i.e. the home directory. The capture therefore always lands at
`<homedir>/screenshots/<name>`; `Claude/wl.py` moves it to the requested location. When running the
binary by hand, pass a plain file name to `--capture` and look in
`<homedir>/screenshots/`.

### Residual nondeterminism

**The chrome is not reproducible, which is why it is hidden by default.** Measured on
`test/maps/plain.wmf`, three runs with identical flags:

| Flags | Result |
|---|---|
| default (chrome hidden) | byte-identical |
| default at a different `--capture-at` | byte-identical |
| default without `--capture-view` | byte-identical |
| `--capture-show-ui` | **all three differ** |

Pixel-diffing two `--capture-show-ui` runs puts the entire difference in the bottom rows of the
window (rows 652-709 of 720, 494 pixels, 0.05%) — the info panel, not the map. The exact widget has
not been pinned down; it is *not* the FPS readout, which is gated on `dfDebug`
(`interactive_base.cc:1035`) and therefore off. `--capture-show-ui` logs a warning to that effect.

This corrects an earlier claim here that there was "none worth chasing with the default flags":
that was written before the behaviour was measured, and it was wrong.

Sources that remain but are not drawn in a default capture: the minimap attack blink
(`minimap_renderer.cc:192`) and the debug-overlay FPS text (`interactive_base.cc:1009`), both
reading `SDL_GetTicks()`. If a capture ever includes them, expect noise.

`--fixed-timestep=0` deliberately re-enables the timing-dependent gametime advance and is the
control for verifying that the mechanism does the work.

### Driver script

`Claude/wl.py` builds the invocation, runs it, and collects the outputs:

```
Claude/wl.py --scenario test/maps/plain.wmf --at 30000 --view 512,512,1.0 --shot out.png
Claude/wl.py --compare a.png b.png      # byte-identical? exit 0 if yes
```

It always passes a fresh sandboxed `--homedir`, `--nosound`, `--play_intro_music=false`, fixed
`--xres`/`--yres` (default 1280x720), `--fail-on-lua-error`, `--fail-on-errors`,
`--messagebox-timeout`, and `--language=en`. It enforces a wall-clock timeout (default 120 s,
`--timeout`), kills hung runs, and exits non-zero on crash, Lua error, timeout, or a missing
screenshot. The output directory (the `--shot` path's directory) gets `run.log` (stdout+stderr),
`cmdline.txt` (the exact invocation, so a run is reproducible by hand), and the PNG. `--compare`
reports whether two captures are byte-identical — the acceptance check for this milestone.

---

## Determinism

For a screenshot to be worth diffing, everything below must be pinned:

| Source of variation | Pinned by |
|---|---|
| Animation phase (trees, water, workers) | `--fixed-timestep` + the logic-thread freeze at an exact tick — see above |
| Camera position and zoom | `--capture-view`, applied with `Transition::Jump` |
| Window size | `--xres` / `--yres` |
| Overlays (census, buildhelp, grid, …) | `--display_flags` |
| Toolbar and info panel | hidden by default in capture mode; `--capture-show-ui` gives that up |
| Mouse cursor | `--sdl_cursor` default (system-drawn, absent from readback) |
| Config drift between runs | `--homedir` sandbox |
| Game state | Fixed savegames — the scene catalog, `backlog.md` 1.7 |

Verified by capturing `test/maps/plain.wmf` twice with identical flags: the PNGs are byte-identical.
Disabling the mechanism (`--fixed-timestep=0`) makes the two runs differ, which confirms the
determinism comes from the timestep rather than luck.

---

## Beyond screenshots

**Image diffing** (`backlog.md` 1.4): a compare step emitting a diff PNG plus pixel statistics, so a
change can be stated as "altered 0.3% of pixels, concentrated at water edges" instead of eyeballed.
`wl.py --compare` already does the byte-identity check; the diff image builds on it.

**Render statistics** (`backlog.md` 1.8): `RenderQueue::draw()` is the single choke point for all
screen drawing (`render_queue.cc:214`). Per-frame item count, batch count, draw calls, opaque vs.
blended split and atlas rebuilds can all be logged from that one place — the metric that says
whether a renderer change did anything.

**Log tags** (`backlog.md` 1.9): retrofitting a category onto `do_log` means touching every call
site. Cheaper: a separate `log_render(...)` macro family for the subsystems in play, plus
`--log-filter=render,gl`, plus `--logfile=PATH` on non-Windows (currently Windows-only,
`src/base/log.h:80-92`) so the file gets everything while the terminal stays usable.

---

## State of play: where the harness is weak

Written after milestone 1 landed and before renderer work started, so the honest assessment does
not get lost. The mechanism works; what follows is about its *fitness for purpose*, not its
correctness.

### The only scene we have is nearly empty

Everything so far has been verified against `test/maps/plain.wmf` at one viewpoint: grass, one
headquarters, a rock face. That exercises almost none of the renderer — no water or terrain
transition edges, no dense animation, no roads or waterways, no zoom extremes, no minimap, no
workarea overlays. A diff against this scene would miss most of what a renderer change touches.

This is the harness's biggest limitation today, and closing it needs no code at all — only picking
scenes and viewpoints (`backlog.md` 1.7). Until it is closed, a green `--compare` means much less
than it appears to.

### Byte identity is a weak answer

`wl.py --compare` reports identical or not identical, nothing more. For renderer work the useful
question is *what changed and where*.

This is not speculative: during the milestone-1 review, byte identity established only that
something varied, and an ad-hoc script computing the differing pixel count and bounding box is what
localized the variation to rows 652-709 — the info panel — which in turn produced the finding that
the chrome had to be hidden by default. The throwaway script did the work that `--compare` could
not. Making it a first-class command is small (`backlog.md` 1.4).

### The capture loop is slow enough to notice

About 7 s per capture on the default Debug + ASan build. With a six-scene catalog that is roughly
45 s per comparison cycle, which is enough to discourage running it. A `RelWithDebInfo` build
directory would cut it substantially (`backlog.md` 0.2). Not urgent while there is one scene.

### What the harness deliberately does not do

No log filtering (`1.9`) and no Lua-driven game control (`1.10`). Both were judged tertiary and
neither has been missed yet. They should be built when a task actually demands them rather than
speculatively — the same reasoning that made the Lua screenshot binding (`1.2`) unnecessary once
the capture turned out to be better done in C++.

---

## Open questions

- Whether a fixed small resolution changes any rendering path in a way that would make captures
  unrepresentative of normal play.

Resolved during Milestone 1:

- **Does `MapView:close()` exit the process cleanly when launched via `--loadgame`?** Yes: the game
  returns from its modal loop, `WLApplication::run()` falls through to `should_die_ = true`, and
  the process exits. This is the exit path the capture state machine uses.
- **How does the layered filesystem resolve a screenshot path?** Every path — absolute ones
  included — is rooted inside the home directory (`FileSystem::canonicalize_name`), so the capture
  is written to `<homedir>/screenshots/<name>` and the driver script moves it out.
