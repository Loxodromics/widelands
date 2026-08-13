#!/usr/bin/env python3
# encoding: utf-8

"""Driver script for deterministic Widelands screenshot captures.

Runs a built Widelands binary against a map or savegame with the dev-harness
capture switches (--capture etc., see src/dev_harness/), in a sandboxed home
directory, and reports success/failure. Two captures of the same command line
must produce byte-identical PNGs; --compare is the verification tool for that.

See Claude/DEV_HARNESS.md for the design.

Usage:
    wl.py --scenario test/maps/plain.wmf --at 30000 --view 512,512,1.0 --shot out.png
    wl.py --compare a.png b.png
"""

from glob import glob
import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile
import time

get_time = time.monotonic

DEFAULT_XRES = 1280
DEFAULT_YRES = 720
DEFAULT_TIMEOUT = 120  # seconds
MESSAGEBOX_TIMEOUT = 20  # seconds


def datadir():
    return os.path.join(os.path.dirname(__file__), "..", "data")


def datadir_for_testing():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def find_binary():
    # Prefer the binary from the build directory; the copy in the repo root is
    # a convenience copy from compile.sh and can easily be stale.
    for potential_binary in (
        glob(os.path.join(os.path.dirname(__file__), "..", "build", "src", "widelands")) +
        glob(os.path.join(os.path.dirname(__file__), "..", "widelands")) +
        glob(os.path.join("build", "src", "widelands"))
    ):
        if os.access(potential_binary, os.X_OK):
            return potential_binary

    # Fall back to binary in $PATH if possible
    return shutil.which("widelands")


def check_binary(binary):
    # Prefer binary from source directory instead of PATH
    if os.path.dirname(binary) == '' and os.access(binary, os.X_OK):
        return os.path.join(os.curdir, binary)

    if shutil.which(binary) is not None:
        return binary

    if os.path.dirname(binary) != '' and os.access(binary, os.X_OK):
        return binary

    for potential_path in [os.curdir, os.path.join(os.path.dirname(__file__), "..")]:
        fullpath = os.path.join(potential_path, binary)
        if os.access(fullpath, os.X_OK):
            return fullpath

    return None


def build_args(args, binary, homedir, shot_name):
    wlargs = [
        binary,
        f'--datadir={datadir()}',
        f'--datadir_for_testing={datadir_for_testing()}',
        f'--homedir={homedir}',
        '--nosound',
        '--play_intro_music=false',
        '--fail-on-lua-error',
        '--fail-on-errors',
        f'--messagebox-timeout={MESSAGEBOX_TIMEOUT}',
        f'--xres={args.xres}',
        f'--yres={args.yres}',
        '--language=en',
    ]
    if args.scenario is not None:
        wlargs.append(f'--scenario={args.scenario}')
    elif args.loadgame is not None:
        wlargs.append(f'--loadgame={args.loadgame}')
    else:
        wlargs.append(f'--editor={args.editor}' if args.editor else '--editor')

    # The game writes the capture into its home directory's screenshots folder
    # under the name given here; we move it to the requested path afterwards.
    wlargs.append(f'--capture={shot_name}')
    if args.at is not None:
        wlargs.append(f'--capture-at={args.at}')
    if args.view is not None:
        wlargs.append(f'--capture-view={args.view}')
    if args.show_ui:
        wlargs.append('--capture-show-ui')
    if args.fixed_timestep is not None:
        wlargs.append(f'--fixed-timestep={args.fixed_timestep}')
    return wlargs


def run_capture(args, binary):
    if args.scenario is None and args.loadgame is None and args.editor is None:
        print("error: exactly one of --scenario, --loadgame or --editor is required")
        return False

    shot_path = os.path.abspath(args.shot)
    shot_name = os.path.basename(shot_path)
    out_dir = os.path.dirname(shot_path)
    if not out_dir:
        out_dir = "."
    try:
        os.makedirs(out_dir, exist_ok=True)
    except OSError as e:
        print(f"error: cannot create output directory {out_dir}: {e}")
        return False

    sandbox = tempfile.mkdtemp(prefix="widelands_capture_")
    try:
        wlargs = build_args(args, binary, sandbox, shot_name)
        # Name the side files after the screenshot so several captures can share
        # one output directory without clobbering each other's log.
        stem = os.path.splitext(shot_name)[0]
        cmdline_path = os.path.join(out_dir, f"{stem}.cmdline.txt")
        with open(cmdline_path, "w") as cmdline:
            cmdline.write(" ".join(wlargs))
            cmdline.write("\n")

        log_path = os.path.join(out_dir, f"{stem}.run.log")
        env = dict(os.environ)
        lsan = env.get("LSAN_OPTIONS", "")
        if "suppressions=" not in lsan:  # allow to overwrite
            lsan += f" suppressions={datadir_for_testing()}/asan_3rd_party_leaks"
        env["LSAN_OPTIONS"] = lsan

        print(f"Running: {' '.join(wlargs)}")
        start = get_time()
        timed_out = False
        with open(log_path, "wb") as log:
            process = subprocess.Popen(wlargs, stdout=log, stderr=subprocess.STDOUT, env=env)
            try:
                process.communicate(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                timed_out = True
        duration = get_time() - start
        print(f"Widelands returned in {duration:.1f}s with code {process.returncode}")

        if timed_out:
            print(f"error: timed out after {args.timeout}s; log in {log_path}")
            return False
        if process.returncode != 0:
            print(f"error: Widelands exited abnormally ({process.returncode}); log in {log_path}")
            return False

        captured = os.path.join(sandbox, "screenshots", shot_name)
        if not os.path.isfile(captured):
            print(f"error: no screenshot written; log in {log_path}")
            return False
        try:
            shutil.move(captured, shot_path)
        except OSError as e:
            print(f"error: cannot move screenshot to {shot_path}: {e}")
            return False
        print(f"Screenshot written to {shot_path}")
        return True
    finally:
        if args.keep_sandbox:
            print(f"Sandbox home kept at {sandbox}")
        else:
            shutil.rmtree(sandbox, ignore_errors=True)


def compare_captures(path_a, path_b):
    for path in (path_a, path_b):
        if not os.path.isfile(path):
            print(f"error: no such file: {path}")
            return False
    if filecmp.cmp(path_a, path_b, shallow=False):
        print("Captures are identical.")
        return True
    print(f"Captures differ: {path_a} vs {path_b}")
    return False


def parse_args():
    p = argparse.ArgumentParser(description="Run a deterministic Widelands screenshot capture.")

    mode = p.add_mutually_exclusive_group(required=False)
    mode.add_argument("--scenario", metavar="FILE", help="Start the map as a scenario.")
    mode.add_argument("--loadgame", metavar="FILE", help="Load the savegame.")
    mode.add_argument("--editor", metavar="FILE", nargs="?", const="", help="Start the editor.")

    p.add_argument("--shot", metavar="PNG", help="Where to write the screenshot (required).")
    p.add_argument("--at", metavar="MS", help="Game time to freeze and capture at (default 0).")
    p.add_argument("--view", metavar="X,Y,ZOOM", help="Map pixel and zoom for the capture camera.")
    p.add_argument("--show-ui", action="store_true",
                   help="Keep toolbar and info panel (hidden by default; showing them makes "
                        "captures differ between runs).")
    p.add_argument("--fixed-timestep", metavar="MS",
                   help="Fixed game time step per logic tick (default in capture mode: 50; "
                        "0 disables it and makes captures timing-dependent).")
    p.add_argument("--compare", metavar="PNG", nargs=2, help="Compare two captures instead.")
    p.add_argument("--xres", type=int, default=DEFAULT_XRES, help=f"Window width (default {DEFAULT_XRES}).")
    p.add_argument("--yres", type=int, default=DEFAULT_YRES, help=f"Window height (default {DEFAULT_YRES}).")
    p.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                   help=f"Wall-clock timeout in seconds (default {DEFAULT_TIMEOUT}).")
    p.add_argument("--binary", help="Path to the widelands binary.")
    p.add_argument("--keep-sandbox", action="store_true", help="Keep the temporary home directory.")

    args = p.parse_args()

    if args.binary is None:
        args.binary = find_binary()
        if args.binary is None:
            p.error("No widelands binary found. Please specify with --binary.")
    else:
        args.binary = check_binary(args.binary)
        if args.binary is None:
            p.error("The specified widelands binary is not found.")

    if args.compare is not None and (args.shot is not None or args.at is not None or
                                     args.view is not None or args.show_ui or
                                     args.fixed_timestep is not None):
        p.error("--compare cannot be combined with capture options.")
    return args


def main():
    args = parse_args()

    if args.compare is not None:
        return 0 if compare_captures(*args.compare) else 1
    if args.shot is None:
        print("error: --shot is required for captures")
        return 1
    return 0 if run_capture(args, args.binary) else 1


if __name__ == '__main__':
    sys.exit(main())
