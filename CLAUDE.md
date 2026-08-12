# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Widelands is a free, open source real-time strategy game (C++17, SDL2, Lua scripting) with a
singleplayer campaign and a deterministic-lockstep multiplayer mode. GPL v2+.

## Contribution policy (important)

The project's CONTRIBUTING.md explicitly states that AI-generated content will not be accepted
as pull requests and may be closed without review. Treat this repo as a place to help the user
work locally (debugging, understanding code, drafting patches for the user to review and rewrite
in their own words) rather than as a target for autonomous PR submission. Never open, comment on,
or push directly to PRs/issues on behalf of the user without their explicit instruction.

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
