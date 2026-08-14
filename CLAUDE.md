# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Widelands is a free, open source real-time strategy game (C++17, SDL2, Lua scripting) with a
singleplayer campaign and a deterministic-lockstep multiplayer mode. GPL v2+.

## Fork context (important)

This checkout is a personal fork (`origin` = `github.com/Loxodromics/widelands`), worked on for fun.
Upstream's CONTRIBUTING.md says AI-generated content will not be accepted as pull requests — that
applies to upstream PRs, not to work done here. Build freely, including C++ changes and dev tooling;
do not hedge or re-litigate the upstream policy. If something useful comes out of it, whether
upstream adopts it is their call.

Still: never push, open PRs, or comment on issues (here or upstream) without explicit instruction.

`Claude/` is the shared workspace for design docs, notes, the backlog (`Claude/backlog.md`), and
dev-harness tooling. Keeping it there also keeps rebases onto upstream clean.

## Build

Two supported paths; either works, use whichever the user already has configured (check for a
`build/` directory first).

### Convenience script

```
./compile.sh          # full debug build (AddressSanitizer on by default)
./compile.sh -r -w     # release build, skip website tools
./compile.sh -h         # all options
```

### CMake directly (Ninja preferred over Unix Makefiles)

```
mkdir build && cd build
cmake -G Ninja ..
ninja
```

Useful `-D` options (Debug build is the default):
- `CMAKE_BUILD_TYPE=Debug|Release|RelWithDebInfo`
- `OPTION_ASAN=ON|OFF` — AddressSanitizer (default ON for Debug, OFF for Release; clearing the
  build dir is required to turn it back off once built)
- `OPTION_BUILD_TESTS=ON|OFF` — build the C++ unit tests (default ON)
- `OPTION_BUILD_CODECHECK=ON|OFF` — build the codecheck style-checker (Debug only, default ON)
- `OPTION_BUILD_WEBSITE_TOOLS=ON|OFF` — build the `wl_map_info`/`wl_map_object_info` etc. tools

The binary ends up at `build/src/widelands` (the convenience script copies it to the repo root).

## Tests

There are two independent test layers:

**C++ unit tests** (framework in `src/base/test.h`, one test binary per module, e.g.
`test_base`, `test_economy`, `test_ai`, `test_luna`): built automatically when
`OPTION_BUILD_TESTS=ON` (default) and registered with CTest.

```
cd build && ctest                      # run all unit tests
cd build && ctest -R test_economy      # run one test binary
./build/src/base/test_base             # run a test binary directly
```

`compile.sh` also links and runs these by default; pass `-s`/`--skip-tests` to skip.

**Lua/map regression tests** (`test/maps`, `test/scripting`, `test/templates`): run against a
built `widelands` binary via the top-level script.

```
./regression_test.py                          # full suite
./regression_test.py -r <regexp>               # filter by test name
./regression_test.py -b path/to/widelands      # use a specific binary
./regression_test.py -j <n>                     # parallel workers
```

## Code style and static checks

- Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
- Formatting is enforced via `.clang-format`; `utils/fix_formatting.py` reformats C++ (clang-format),
  Lua, and Python (autopep8/autoflake/unify) — bunnybot also auto-formats mirrored branches, so this
  is a convenience, not a strict prerequisite for local iteration.
- `codecheck` is an in-tree style linter (`cmake/codecheck/`) with its own rule tests
  (`cmake/codecheck/run_tests.py`) and a Ninja/Make target: `ninja codecheck` / `make codecheck`.
- `clang-tidy` is run via `utils/run-clang-tidy.py` against a build configured with
  `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`; to check one rule: `python3 utils/run-clang-tidy.py -checks=-*,my-check-prefix*`.
- `///` for short comments, `/* */` block style for anything longer than ~3 lines — already the
    codebase's own convention (e.g. `src/logic/map_objects/tribes/building.h`).
- Comments explain WHY (business decision, non-obvious constraint), not WHAT.
- Write comments as "we", not "I" or passive voice.
- `kPascalCase` for constants — already used (`k100PercentAsInt` in `src/base/math.h`).
- `const`/`constexpr` wherever applicable.
- Prefer `std::unique_ptr` by default; `std::shared_ptr` only for genuine shared ownership —
  already the codebase's ratio (roughly 5:1 unique:shared in `src/logic`).
- `[[nodiscard]]` for validation/resource-acquisition functions where ignoring the result is a
  bug; skip it on simple getters — already used ~1900 times in the tree.
- Pointer-to-type spacing (`Type* ptr`) — matches `.clang-format`'s `PointerBindsToType: true`.
- Descriptive names; discourage `auto` except where it genuinely helps readability.
- Log sparingly, at key decision points and error conditions, with an appropriate level.
- Header/cpp side-by-side under `src/` — already how the tree is laid out.

## Architecture

Source lives under `src/`, one directory per subsystem (see `src/README.md`):

| Directory | Contents |
| --- | --- |
| `logic` | Core game simulation: map, player, tribes, world (`logic/map_objects/tribes`, `logic/map_objects/world`) |
| `commands` | `Command`/`GameLogicCommand`/`PlayerCommand` objects — see below |
| `economy` | Economy simulation (wares, routing, roads/waterways) layered on `logic` |
| `ai` | Computer player implementations |
| `network` | Multiplayer networking backend |
| `game_io` | Savegame serialization |
| `map_io` | Map file serialization |
| `editor` | The in-game map editor |
| `ui` | Widelands UI toolkit and screens |
| `graphic` | Rendering backend: image loading, font rendering, animations, UI templating |
| `scripting` | Lua scripting interface (campaigns/scenarios call into this) |
| `sound`, `chat`, `io`, `base` | Audio, chat backend, filesystem/file-format helpers, low-level utilities (i18n, vectors, logging, macros) |
| `notifications` | Observer-pattern pub/sub used to decouple subsystems |
| `third_party` | Vendored deps: eris, gettext, libmd, minizip |
| `website` | Standalone tools consumed by the widelands-website project |

### Deterministic simulation / network model

Multiplayer uses lockstep: all clients simulate the same game state and only exchange commands,
not state. `commands/command.h` defines the base `Command` (used for network/orchestration events
that are *not* part of the simulation and are not saved) versus `GameLogicCommand`/`PlayerCommand`
subclasses (in `src/commands/cmd_*.cc`, e.g. `cmd_build_building.cc`, `cmd_ship_sink.cc`) which
*are* part of the simulation, get scheduled by due-time, saved in savegames, and must execute
identically on every client. When touching gameplay-affecting code, keep an eye on whether it
needs to go through this command queue rather than mutating state directly, or you will break
network sync and replays (`logic/replay.h`).

### Save/load split

Two independent serialization layers: `map_io` (the map itself — terrain, tribes placement, etc.,
used by both the editor and the game) and `game_io` (full running-game state — savegames).

### Misc
The dirctory "Claude" is its own repo
