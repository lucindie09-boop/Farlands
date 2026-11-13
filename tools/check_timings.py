#!/usr/bin/env python3
"""Wall-clock regression gate for the three timings a player feels.

Boot, world fill and the fps settle are not unit tests: they need a window, a GPU
and a save, so this is a local gate (`scons timings`), not a CI job. Each metric
is compared against a ceiling in `timings_baseline.txt`, the same format the C++
benchmark uses, because wall-clock here carries a machine's own noise: the
ceilings are several times the desktop measurement, so only a regression the size
of the cloud-layer stall (4.2 s in a 1 s boot) can trip them.

The world is generated in an isolated APPDATA, so a run never reads or writes the
player's save, and two runs see the same fresh world rather than whatever was
streamed last session. The fill and settle probes are also given a fixed render
distance (`RD`, 32 by default, `--rd` to change it): fill time is a function of
how many chunks the ring walk has to produce, so letting it follow the world's
own setting would compare two different loads.

    python tools/check_timings.py                 # check against the baseline
    python tools/check_timings.py --write         # print ceilings from this run
    python tools/check_timings.py --runs 3        # median of three launches
    python tools/check_timings.py --godot /path/to/godot
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "timings_baseline.txt"
DEFAULT_GODOT = Path(
    r"C:\Users\lucin\Documents\Developement\Godot\Godot_v4.7.1-stable_win64_console.exe"
)
# Ceilings sit this far above what the run measured: the spread between two
# consecutive launches on this desktop is already tens of milliseconds.
CEILING_FACTOR = 1.5

BOOT = "boot_engine_ms"
TITLE = "boot_title_ms"
WORLD = "boot_world_ms"
FILL = "fill_settle_s"
SETTLE = "fps_settle_s"


def find_godot(explicit):
    for candidate in (explicit, os.environ.get("GODOT"), str(DEFAULT_GODOT)):
        if candidate and Path(candidate).exists():
            return str(candidate)
    sys.exit("check_timings: no Godot binary found; pass --godot or set GODOT")


def run_probe(godot, script, env_extra, timeout):
    """Runs one probe in a fresh user-data directory and returns its stdout."""
    with tempfile.TemporaryDirectory(prefix="farlands-timings-") as user_dir:
        env = dict(os.environ)
        env.update(env_extra)
        # Godot's user:// follows APPDATA on Windows and XDG_DATA_HOME elsewhere.
        env["APPDATA"] = user_dir
        env["XDG_DATA_HOME"] = user_dir
        print("  %s" % script, flush=True)
        try:
            done = subprocess.run(
                [godot, "--path", str(ROOT), "--script", "res://probes/%s" % script],
                capture_output=True, text=True, errors="replace",
                timeout=timeout, env=env,
            )
        except subprocess.TimeoutExpired:
            sys.exit("check_timings: %s did not finish in %d s" % (script, timeout))
        if done.returncode != 0:
            print(done.stdout[-2000:])
            sys.exit("check_timings: %s exited %d" % (script, done.returncode))
        if re.search(r"^PROBE .*FAILED", done.stdout, re.M):
            print(done.stdout[-2000:])
            sys.exit("check_timings: %s reported FAILED" % script)
        return done.stdout


def one_boot(godot):
    out = run_probe(godot, "probe_launch.gd", {"SECS": "3"}, 90)
    engine = re.search(r"engine_ms=(\d+)", out)
    title = re.search(r"title_ms=[\d.]+ uptime_ms=(\d+)", out)
    world = re.search(r"world_ms=[\d.]+ uptime_ms=(\d+)", out)
    if not (engine and title and world):
        sys.exit("check_timings: probe_launch printed no summary\n" + out[-1500:])
    return {BOOT: int(engine.group(1)), TITLE: int(title.group(1)), WORLD: int(world.group(1))}


def require_rd(out, rd, script):
    """The probes print the render distance they actually used; a probe that
    ignores RD would otherwise report a different load under the same name."""
    found = re.search(r"\brd=(\d+)", out)
    if not found or int(found.group(1)) != rd:
        sys.exit("check_timings: %s did not run at rd=%d\n%s" % (script, rd, out[-1200:]))


def measure(godot, runs, rd):
    print("boot (%d run(s), isolated user dir):" % runs)
    boots = [one_boot(godot) for _ in range(runs)]
    values = {key: statistics.median([b[key] for b in boots]) for key in (BOOT, TITLE, WORLD)}

    print("fill (rd=%d):" % rd)
    out = run_probe(godot, "probe_start_fill.gd",
                    {"PROBE_TIMEOUT_SECS": "120", "RD": str(rd)}, 180)
    require_rd(out, rd, "probe_start_fill.gd")
    settle = re.search(r"PROBE fill settle t=([\d.]+)s", out)
    if not settle:
        sys.exit("check_timings: the fill never settled in 120 s\n" + out[-1500:])
    values[FILL] = float(settle.group(1))

    print("fps settle (rd=%d):" % rd)
    out = run_probe(godot, "probe_fps_settle.gd", {"SECS": "12", "RD": str(rd)}, 120)
    require_rd(out, rd, "probe_fps_settle.gd")
    settle = re.search(r"settle_1s=([-\d.]+)s", out)
    if not settle or float(settle.group(1)) < 0.0:
        sys.exit("check_timings: the frame rate never settled in 12 s\n" + out[-1500:])
    values[SETTLE] = float(settle.group(1))
    return values


def load_baseline(path):
    if not path.exists():
        return {}
    ceilings = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) == 2:
            ceilings[parts[0]] = float(parts[1])
    return ceilings


def compare(values, ceilings):
    """Returns (report lines, regression count). Kept separate from the probes so
    `--selftest` can prove the comparison fires without launching a game."""
    lines = []
    regressions = 0
    for key, value in values.items():
        ceiling = ceilings.get(key)
        if ceiling is None:
            lines.append("  SKIP %s: no baseline entry (measured %.2f)" % (key, value))
            continue
        if value > ceiling:
            lines.append("  REGRESSION %s: %.2f > %.2f" % (key, value, ceiling))
            regressions += 1
        else:
            lines.append("  OK %s: %.2f <= %.2f" % (key, value, ceiling))
    return lines, regressions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", help="path to the Godot binary")
    parser.add_argument("--runs", type=int, default=2, help="launches to median (default 2)")
    parser.add_argument("--rd", type=int, default=32, help="render distance for fill/settle (default 32)")
    parser.add_argument("--write", action="store_true", help="print baseline lines, do not check")
    parser.add_argument("--selftest", action="store_true", help="prove the comparison fires")
    args = parser.parse_args()

    if args.selftest:
        passing, ok = compare({BOOT: 900.0}, {BOOT: 1550.0})
        failing, bad = compare({BOOT: 1600.0}, {BOOT: 1550.0})
        missing, skipped = compare({BOOT: 900.0}, {})
        if ok != 0 or bad != 1 or skipped != 0 or "SKIP" not in missing[0]:
            print("FAILED: pass=%d fail=%d skip=%d" % (ok, bad, skipped))
            return 1
        print("check_timings selftest OK: pass, fail and missing-entry paths fire")
        return 0

    godot = find_godot(args.godot)
    values = measure(godot, max(1, args.runs), args.rd)

    if args.write:
        print("\n--- ceilings at %.1fx this run ---" % CEILING_FACTOR)
        for key, value in values.items():
            print("%s %.2f" % (key, value * CEILING_FACTOR))
        return 0

    ceilings = load_baseline(BASELINE)
    if not ceilings:
        sys.exit("check_timings: %s has no entries" % BASELINE)
    print("\n--- ceiling check (%s) ---" % BASELINE.name)
    lines, regressions = compare(values, ceilings)
    print("\n".join(lines))
    if regressions:
        print("\nFAILED: %d timing(s) regressed beyond the baseline." % regressions)
        return 1
    print("\nAll timings within baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
