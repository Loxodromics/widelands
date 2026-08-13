# Backlog

Working backlog for the fork. See `CLAUDE.md` for fork context; `RENDERER.md` for how the renderer
is put together.

Status markers: `[ ]` open, `[~]` in progress, `[x]` done, `[?]` needs a decision first.

---

## 0. Repo hygiene

### [?] Rebasing regularly onto upstream

Not solved yet — needs a decision before it becomes painful. Current state:

- `origin` = `git@github.com:Loxodromics/widelands.git` (the fork). **No `upstream` remote is
  configured at all**, so we currently have no way to even see what upstream has moved on to.
- First concrete step regardless of strategy:
  `git remote add upstream https://github.com/widelands/widelands.git` (URL to be confirmed).

Open questions to settle:

- **Branch layout.** Keep `master` as a clean mirror of upstream and do all our work on a long-lived
  branch, or carry our work directly on `master`? The mirror approach makes `git rebase upstream/master`
  mechanical and keeps the diff against upstream legible at all times. The cost is that `origin/master`
  then no longer holds our work.
- **Rebase vs. merge.** Rebasing keeps a clean, reviewable diff against upstream but rewrites history,
  which means force-pushing to the fork. Merging avoids that but the diff gets muddy over time.
- **Cadence.** On demand, or on a schedule? Upstream is reasonably active.
- **Conflict surface.** The smaller our footprint in `src/`, the cheaper every rebase is. This is the
  main argument for the structure below.

Mitigation that applies whatever we choose: keep everything we add **out of the upstream tree where
possible**. Docs, harness scripts and Lua helpers live in `Claude/`, which upstream will never touch.
Keep the C++ additions few, small, and surgical, each in its own commit with a clear message, so they
can be individually replayed, squashed or dropped.

### [ ] Pre-existing regression-suite failure (not ours)

`./regression_test.py` runs 76/77 green. The one failure is
`test/maps/market_trading.wmf/scripting/test_simple_trade.lua`, an ASan container-overflow at
`carrytradeitem.cc:136` in `Widelands::Worker::carry_trade_item_update`.

Confirmed pre-existing, not caused by the dev harness: it reproduces identically on a binary built
before `src/dev_harness/` existed, and consistently rather than flakily. Recorded here so the next
person to run the suite does not go looking for it in our changes. Unrelated to the harness; fix or
report upstream if it becomes annoying.

### [ ] Second build directory for visual iteration

The default debug build has AddressSanitizer on — fine for correctness, too slow for screenshot
iteration. Set up `build-rel/` on `RelWithDebInfo` with `OPTION_ASAN=OFF`, keep `build/` as-is.

---

## 1. Dev harness for renderer work

Get from a shell command to a deterministic, diffable screenshot of a known scene, without touching
menus. **Design and reference: `DEV_HARNESS.md`** — it records what the CLI and Lua API already
provide, so we don't rebuild any of it. Items below are ordered so each is useful before the next
exists.

- [x] **1.1 Driver script `Claude/wl.py`** — sandboxed homedir, timeout, known log and screenshot
      paths, non-zero exit on Lua error. Pure Python; built on `--script`-free capture switches
      (see 1.2–1.6). Landed together with the capture support in `src/dev_harness/`.
- [x] **1.2 Screenshot from Lua** — not needed as designed: the capture is C++-driven instead
      (`--capture`, `DevHarness::Capture`), which also sidesteps the logic-thread/UI-thread
      problem. `Graphic::screenshot_pending()` was added as part of it (G1 in `DEV_HARNESS.md`).
- [x] **1.3 Freeze the animation clock** — `--fixed-timestep=<ms>` (default 50 in capture mode).
      The original claim that animations run in real time was wrong: they are gametime-driven
      (`bob.cc:824`, `immovable.cc:434`); the real problem was the real-time-derived gametime
      advance, which the fixed timestep pins. The simulation is then frozen at an exact tick from
      the logic thread (G2 in `DEV_HARNESS.md`).
- [ ] **1.4 Image diffing** — diff PNG plus pixel statistics. `wl.py --compare` does the
      byte-identity check; the diff image still to come.
- [x] **1.5 Viewpoint and zoom from Lua** — superseded by `--capture-view`, applied with
      `Transition::Jump` from C++ (G3 in `DEV_HARNESS.md`).
- [x] **1.6 Hide the GUI** — via `InteractiveBase::set_chrome_visible` (G4 in `DEV_HARNESS.md`).
      Hiding is the *default* in capture mode: the info panel draws real-time dependent content, so
      a capture including the chrome is not reproducible. `--capture-show-ui` opts out and warns.
- [ ] **1.7 Scene catalog** — 4–6 committed savegames with fixed viewpoints covering the renderer
      surface. Determinism is in place, the scenes still need choosing.
- [ ] **1.8 Render statistics to the log** — instrument `RenderQueue::draw()`, the single choke
      point for all screen drawing.
- [ ] **1.9 Log tags and filtering** — deferred; a separate `log_render(...)` family plus
      `--log-filter`, and `--logfile` on non-Windows.
- [ ] **1.10 Game control from Lua** — tertiary, mostly exists already.

---

## 2. Open questions to verify

- Upstream remote URL for widelands (see section 0).

Resolved during milestone 1:

- Does `MapView:close()` exit the process cleanly, or drop to the main menu, when launched via
  `--loadgame`? — **exits the process** (see `DEV_HARNESS.md`, Open questions).
- Screenshot path resolution through the layered filesystem (see 1.2) — **absolute paths are
  rooted inside the home directory**; captures land in `<homedir>/screenshots/` and `wl.py` moves
  them out.

---

## 3. Renderer work

Tracked separately in `RENDERER.md` and `RENDERER_CODE_REVIEW.md`. Items graduate from there into
this backlog once we decide to act on them.
