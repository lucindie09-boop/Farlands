#!/usr/bin/env python3
"""Five paired start-fill runs of the current build.

A is the normal render loop; B is NO_RENDER=1, the arm whose 5.19/5.51 s spread
against the normal 5.48-5.57 s range is the reason for a paired design. The arm
order alternates by pair (AB, BA, AB, BA, ...) so a drift or a warming cache
across the session cannot favor one arm the way a fixed ABAB order would.

Each run is probes/probe_start_fill.gd at RD 32 through probes/run_probe_shot.sh;
the script keeps every log, writes results.csv as it goes, and reports a median
and a min-max range per arm plus the paired A-B differences. The optional warmup
run is reported but never counted.
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "probes" / "run_probe_shot.sh"
PROBE = "probes/probe_start_fill.gd"

LOADED_RE = re.compile(
    r"PROBE fill loaded=([0-9.]+)s mesh=([0-9.]+)s far=([0-9.]+)s "
    r"end=([0-9.]+)s samples=(\d+) frames=(\d+) worst=([0-9.]+)ms slow=(\d+)"
)
FRAME_RE = re.compile(r"PROBE fill frame avg=[0-9.]+ms load_avg=([0-9.]+)ms")
PROCESS_RE = re.compile(r"PROBE fill phase process_total total=([0-9.]+)s")

FIELDS = [
    "pair", "order", "arm", "label", "loaded_s", "mesh_s", "far_s", "end_s",
    "tail_s", "samples", "frames", "load_avg_ms", "process_total_s", "worst_ms",
    "slow", "run_s", "log",
]

# Probe switches that would make one run incomparable with another. The two arms
# differ only in NO_RENDER; everything else is the build's default.
CLEAR_ENV = (
    "NO_RENDER", "QUIET", "QUIET_MATCH", "SCRIPT_COST",
    "FARLANDS_INSTALL_PROFILE", "FARLANDS_LIGHT_PROFILE",
    "FARLANDS_NO_FRAME_LIMITS", "FARLANDS_REGION_UPLOAD_CAP",
    "FARLANDS_FAR_REGION_DEBOUNCE_MS", "FARLANDS_REGION_SPIKE",
)


def find_bash() -> str:
    candidates = [
        os.environ.get("BASH_EXE"),
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files\Git\usr\bin\bash.exe",
        shutil.which("bash"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return "bash"


BASH = find_bash()


def git(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def parse_output(text: str) -> dict | None:
    matches = LOADED_RE.findall(text)
    if not matches:
        return None
    frame = FRAME_RE.search(text)
    process = PROCESS_RE.search(text)
    if frame is None or process is None:
        return None
    match = matches[-1]
    loaded = float(match[0])
    end = float(match[3])
    return {
        "loaded_s": loaded,
        "mesh_s": float(match[1]),
        "far_s": float(match[2]),
        "end_s": end,
        "tail_s": max(0.0, end - loaded),
        "samples": int(match[4]),
        "frames": int(match[5]),
        "worst_ms": float(match[6]),
        "slow": int(match[7]),
        "load_avg_ms": float(frame.group(1)),
        "process_total_s": float(process.group(1)),
    }


def run_one(label: str, arm: str, out_dir: Path, timeout_s: int, base_env: dict) -> dict | None:
    env = dict(base_env)
    for key in CLEAR_ENV:
        env.pop(key, None)
    env["RD"] = "32"
    if arm == "B":
        env["NO_RENDER"] = "1"

    description = "NO_RENDER=1" if arm == "B" else "render loop on"
    log_path = out_dir / f"{label}.log"
    print(f"[{label}] {description} ... ", end="", flush=True)
    started = time.monotonic()
    try:
        proc = subprocess.run(
            [BASH, str(RUNNER), PROBE, str(timeout_s)],
            cwd=ROOT,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout_s + 60,
        )
        text = proc.stdout or ""
        code = proc.returncode
    except subprocess.TimeoutExpired as exc:
        text = exc.stdout or ""
        if isinstance(text, bytes):
            text = text.decode("utf-8", "replace")
        code = -1
    except OSError as exc:
        text = f"harness: {exc}\n"
        code = -2

    log_path.write_text(text, encoding="utf-8", errors="replace")
    elapsed = time.monotonic() - started
    row = parse_output(text)
    if code != 0 or row is None or "PROBE fill OK" not in text:
        print(f"FAILED (rc={code}, {elapsed:.0f}s)")
        print("\n".join(text.splitlines()[-25:]))
        return None

    row["run_s"] = elapsed
    row["log"] = str(log_path)
    print(f"loaded={row['loaded_s']:.2f}s end={row['end_s']:.2f}s ({elapsed:.0f}s)")
    return row


def append_row(path: Path, row: dict) -> None:
    exists = path.exists()
    with path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS, extrasaction="ignore")
        if not exists:
            writer.writeheader()
        writer.writerow({key: row.get(key, "") for key in FIELDS})


def median_range(values: list[float]) -> tuple[float, float, float]:
    ordered = sorted(values)
    return statistics.median(ordered), ordered[0], ordered[-1]


def metric_block(lines: list[str], label: str, unit: str, digits: int,
                 a_rows: list[dict], b_rows: list[dict], key: str) -> None:
    a_med, a_lo, a_hi = median_range([float(row[key]) for row in a_rows])
    b_med, b_lo, b_hi = median_range([float(row[key]) for row in b_rows])
    lines.append(
        f"{label:<14} A {a_med:.{digits}f}{unit} [{a_lo:.{digits}f}-{a_hi:.{digits}f}]   "
        f"B {b_med:.{digits}f}{unit} [{b_lo:.{digits}f}-{b_hi:.{digits}f}]"
    )


def diff_block(lines: list[str], label: str, unit: str, digits: int,
               pairs: list[tuple[int, dict, dict]], key: str) -> list[float]:
    diffs = [float(a[key]) - float(b[key]) for _, a, b in pairs]
    med, low, high = median_range(diffs)
    faster = sum(1 for value in diffs if value > 0.0)
    slower = sum(1 for value in diffs if value < 0.0)
    ties = len(diffs) - faster - slower
    lines.append(
        f"{label:<14} A-B median {med:+.{digits}f}{unit} "
        f"[{low:+.{digits}f}-{high:+.{digits}f}]   "
        f"B faster {faster}/{len(diffs)} (A faster {slower}, tie {ties})"
    )
    return diffs


def build_summary(args: argparse.Namespace, out_dir: Path, commit: str,
                  dirty: str, warmup: dict | None,
                  pairs: list[tuple[int, dict, dict]]) -> str:
    lines = [
        "Start-fill AB/BA harness",
        f"commit: {commit or 'unknown'}",
        f"tracked dirty: {dirty or 'clean'}",
        "A: normal render loop",
        "B: NO_RENDER=1",
        f"pairs: {args.pairs}, arm order alternates per pair (AB, BA, AB, ...)",
        f"timeout: {args.timeout}s per run",
        f"output: {out_dir}",
        "",
    ]
    if warmup is not None:
        lines.append(
            f"warmup (discarded): loaded={warmup['loaded_s']:.2f}s "
            f"end={warmup['end_s']:.2f}s log={warmup['log']}"
        )
    if not pairs:
        lines.append("no complete pairs")
        return "\n".join(lines)

    a_rows = [a for _, a, _ in pairs]
    b_rows = [b for _, _, b in pairs]
    lines.append("Per arm, median [min-max]:")
    metric_block(lines, "loaded", "s", 2, a_rows, b_rows, "loaded_s")
    metric_block(lines, "end", "s", 2, a_rows, b_rows, "end_s")
    metric_block(lines, "tail", "s", 2, a_rows, b_rows, "tail_s")
    metric_block(lines, "frames", "", 0, a_rows, b_rows, "frames")
    metric_block(lines, "load_avg", "ms", 2, a_rows, b_rows, "load_avg_ms")
    metric_block(lines, "process_total", "s", 2, a_rows, b_rows, "process_total_s")
    metric_block(lines, "worst frame", "ms", 2, a_rows, b_rows, "worst_ms")
    metric_block(lines, "slow frames", "", 0, a_rows, b_rows, "slow")

    lines += ["", "Paired differences (positive = B loaded/finished sooner):"]
    diff_block(lines, "loaded", "s", 2, pairs, "loaded_s")
    diff_block(lines, "end", "s", 2, pairs, "end_s")
    diff_block(lines, "tail", "s", 2, pairs, "tail_s")

    lines += ["", "Pairs:"]
    for number, a, b in pairs:
        lines.append(
            f"  {number}  A {a['loaded_s']:.2f}s  B {b['loaded_s']:.2f}s  "
            f"A-B {a['loaded_s'] - b['loaded_s']:+.2f}s  "
            f"(A end {a['end_s']:.2f}s, B end {b['end_s']:.2f}s)"
        )
    lines += ["", "Logs are in the output directory; results.csv has every field."]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", type=int, default=5, help="counted AB/BA pairs (default 5)")
    parser.add_argument("--timeout", type=int, default=120, help="shell timeout per probe run (default 120)")
    parser.add_argument("--out", default=None, help="output directory (default .freebuff/start_fill_ab_<stamp>)")
    parser.add_argument("--no-warmup", action="store_true", help="skip the discarded warmup run")
    args = parser.parse_args(argv)
    if args.pairs < 1:
        parser.error("--pairs must be at least 1")

    stamp = time.strftime("%Y%m%d_%H%M%S")
    out_dir = Path(args.out) if args.out else ROOT / ".freebuff" / f"start_fill_ab_{stamp}"
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    commit = git("rev-parse", "--short", "HEAD")
    dirty = ", ".join(git("diff", "--name-only").splitlines())
    started = time.strftime("%Y-%m-%d %H:%M:%S")
    meta = [
        f"started: {started}",
        f"commit: {commit or 'unknown'}",
        f"tracked dirty: {dirty or 'clean'}",
        "A: normal render loop",
        "B: NO_RENDER=1",
        f"pairs: {args.pairs}",
        f"warmup: {'no' if args.no_warmup else 'yes'}",
        f"timeout: {args.timeout}s",
        f"bash: {BASH}",
    ]
    (out_dir / "meta.txt").write_text("\n".join(meta) + "\n", encoding="utf-8")
    print("Start-fill AB/BA harness")
    print("\n".join(meta))
    print()

    csv_path = out_dir / "results.csv"
    base_env = os.environ.copy()
    warmup: dict | None = None
    pairs: list[tuple[int, dict, dict]] = []

    if not args.no_warmup:
        warmup = run_one("warmup", "A", out_dir, args.timeout, base_env)
        if warmup is None:
            summary = build_summary(args, out_dir, commit, dirty, None, [])
            (out_dir / "summary.txt").write_text(summary + "\n", encoding="utf-8")
            print("\n" + summary)
            return 1
        warmup.update(pair=0, order="warmup", arm="A", label="warmup")
        append_row(csv_path, warmup)

    for number in range(1, args.pairs + 1):
        order = ("A", "B") if number % 2 == 1 else ("B", "A")
        print(f"\nPair {number}/{args.pairs} order {''.join(order)}")
        pair_rows: dict[str, dict] = {}
        for arm in order:
            label = f"{arm.lower()}{number}"
            row = run_one(label, arm, out_dir, args.timeout, base_env)
            if row is None:
                print(f"\nPair {number} aborted; completed pairs are in the summary.")
                summary = build_summary(args, out_dir, commit, dirty, warmup, pairs)
                (out_dir / "summary.txt").write_text(summary + "\n", encoding="utf-8")
                print("\n" + summary)
                return 1
            row.update(pair=number, order="".join(order), arm=arm, label=label)
            pair_rows[arm] = row
            append_row(csv_path, row)
        pairs.append((number, pair_rows["A"], pair_rows["B"]))

    summary = build_summary(args, out_dir, commit, dirty, warmup, pairs)
    (out_dir / "summary.txt").write_text(summary + "\n", encoding="utf-8")
    print("\n" + summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
