#!/usr/bin/env python3
"""Five paired start-fill runs of the current build.

A is the normal render loop; B is NO_RENDER=1, the arm whose 5.19/5.51 s spread
against the normal 5.48-5.57 s range is the reason for a paired design. The arm
order alternates by pair (AB, BA, AB, BA, ...) so a drift or a warming cache
across the session cannot favor one arm the way a fixed ABAB order would.

With --latch the arms are the initial-loading latch itself: A is the shipping
30 s cap with the 1 s quiet exit, B is the same 1 s quiet exit with no cap
(max=1e9). Both fly the player at 100 blocks/s for --flight seconds as soon as
the latch raises the mesh budget, which keeps mesh work pending so the quiet
exit cannot fire; A therefore ends at the cap and B stays latched for the whole
flight unless the world really does settle. The probe reports the latch's on/end
time, the frame distribution, and the last six seconds (after A's cap, still
inside B's latch).

With --cores N and --vsync the whole session runs pinned to N logical processors
with the display's vsync on: the constrained-hardware arm, where the loading
budget's main-thread work competes with fewer cores and a 60 Hz frame. With
--tail-budget-ms N every run sets FARLANDS_LOADING_TAIL_BUDGET_MS=N, which caps
the three post-load clocks (the shipping 1000 ms is effectively open).

Each run is probes/probe_start_fill.gd at RD 32 through probes/run_probe_shot.sh;
the script keeps every log, writes results.csv as it goes, and reports a median
and a min-max range per arm plus the paired A-B differences. The optional warmup
run is reported but never counted.

With --hold the arms are the initial mesh hold itself: A is the shipping hold,
B is `FARLANDS_HOLD_MESH_UNTIL_LOADED=0`; both render normally.

With --hold-partial N both arms keep the hold; A is the all-or-nothing hold and
B allows N nearest chunk rebuilds/uploads per frame through it
(`FARLANDS_HOLD_MESH_PARTIAL=N`). It measures first-visible against end time.

With --final-install-ms N both arms keep the same build; A restores the 0.75 ms
completion budget and B allows N ms once only the last <=192 installs remain
(`FARLANDS_FINAL_INSTALL_BUDGET_MS`). It measures the final-wave settle.
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
FIRST_RE = re.compile(
    r"PROBE fill first chunk_load=(-?[0-9.]+)s upload=(-?[0-9.]+)s "
    r"visible=(-?[0-9.]+)s visible50=(-?[0-9.]+)s visible90=(-?[0-9.]+)s target=(\d+)"
)
CACHE_RE = re.compile(
    r"PROBE fill cache open=(\d) hits=(\d+) misses=(\d+) hit_rate=([0-9.]+) "
    r"packs=(\d+) entries=(\d+) read_ms=([0-9.]+) decode_ms=([0-9.]+) "
    r"staged=(\d+) written=(\d+) packs_written=(\d+) merged=(\d+) "
    r"write_mb=([0-9.]+) encode_ms=([0-9.]+) write_ms=([0-9.]+) "
    r"encode_failures=(\d+) write_failures=(\d+)"
)
# The probe prints this header when the flight starts and again, with frames=,
# when it ends; requiring frames= keeps the header out of the match.
LATCH_RE = re.compile(
    r"PROBE latch start=([0-9.]+)s flight=([0-9.]+)s frames=(\d+) mean=([0-9.]+)ms "
    r"p99=([0-9.]+)ms worst=([0-9.]+)ms slow=(\d+) on=([0-9.]+)s end=(-?[0-9.]+)s "
    r"gens=(\d+) late_frames=(\d+) late_mean=([0-9.]+)ms late_p99=([0-9.]+)ms "
    r"late_worst=([0-9.]+)ms late_slow=(\d+) budget=(\d+)->(\d+) backlog=(\d+) reasons=(\d+) visible=(\d+) "
    r"visible_tiers=(\d+)/(\d+)/(\d+) visible_bands=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+) "
    r"far_geometry=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)"
    r" far_visible=(\d+)/(\d+)/(\d+)"
    r" far_screen=(\d+)/(\d+)/([0-9.]+)"
)

FIELDS = [
    "pair", "order", "arm", "label", "loaded_s", "mesh_s", "far_s", "end_s",
    "settle_s", "tail_s", "samples", "frames", "load_avg_ms", "process_total_s", "worst_ms",
    "slow", "chunk_load_s", "first_upload_s", "first_visible_s", "visible_50_s",
    "visible_90_s", "visible_target", "run_s", "log",
    "latch_on_s", "latch_end_s", "latch_frames", "latch_mean_ms", "latch_p99_ms",
    "latch_worst_ms", "latch_slow", "late_frames", "late_mean_ms", "late_p99_ms",
    "late_worst_ms", "late_slow", "gens", "budget_start", "budget_end",
    "backlog_end", "reasons_end", "visible_backlog_end",
    "visible_full", "visible_mid", "visible_far",
    "visible_band_0", "visible_band_1", "visible_band_2",
    "visible_band_3", "visible_band_4", "visible_band_5",
    "far_total", "far_no_chunk", "far_empty", "far_no_geometry",
    "far_solid", "far_enclosed", "far_hidden", "far_drawn_chunk",
    "far_drawn_region", "far_hole_total", "far_hole_visible", "far_hole_occluded",
    "far_screen_visible", "far_screen_pixels", "far_screen_coverage",
    "cache_open", "cache_hits", "cache_misses", "cache_hit_rate", "cache_packs",
    "cache_entries", "cache_staged", "cache_written", "cache_packs_written",
    "cache_merged", "cache_write_mb", "cache_read_ms", "cache_decode_ms",
    "cache_encode_ms", "cache_write_ms", "cache_encode_failures",
    "cache_write_failures",
]

# Probe switches that would make one run incomparable with another. The two arms
# differ only in NO_RENDER; everything else is the build's default.
CLEAR_ENV = (
    "NO_RENDER", "QUIET", "QUIET_MATCH", "SCRIPT_COST",
    "FARLANDS_INSTALL_PROFILE", "FARLANDS_LIGHT_PROFILE",
    "FARLANDS_NO_FRAME_LIMITS", "FARLANDS_REGION_UPLOAD_CAP",
    "FARLANDS_FAR_REGION_DEBOUNCE_MS", "FARLANDS_REGION_SPIKE",
    "FARLANDS_LOADING_LATCH_MAX_SEC", "FARLANDS_LOADING_LATCH_QUIET_SEC",
    "FARLANDS_LOADING_TAIL_BUDGET_MS", "FARLANDS_HOLD_MESH_UNTIL_LOADED",
    "FARLANDS_HOLD_MESH_MAX_SEC", "FARLANDS_HOLD_MESH_PARTIAL",
    "FARLANDS_MESH_UPLOADS_LOADING",
    "FARLANDS_FINAL_INSTALL_BUDGET_MS",
    "FARLANDS_WORKER_COUNT", "FILL_TRACE", "LATCH_FLIGHT", "PROBE_TIMEOUT_SECS", "VSYNC_ON",
    "FARLANDS_TERRAIN_CACHE",
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


def pin_to_cores(count: int) -> int:
    """Pin this process -- and the Godot processes it spawns, which inherit the
    mask -- to the first `count` logical processors. Windows-only; returns the
    mask (0 when not pinning)."""
    if count <= 0:
        return 0
    if sys.platform != "win32":
        raise SystemExit("--cores is Windows-only: the harness relies on children inheriting the mask")
    available = os.cpu_count() or 0
    if count > available:
        raise SystemExit(f"--cores {count} exceeds the {available} logical processors available")
    import ctypes
    kernel32 = ctypes.windll.kernel32
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    kernel32.SetProcessAffinityMask.restype = ctypes.c_int
    mask = (1 << count) - 1
    if not kernel32.SetProcessAffinityMask(kernel32.GetCurrentProcess(), mask):
        raise SystemExit(f"could not pin to {count} cores (mask 0x{mask:x})")
    return mask


def git(*args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def parse_output(text: str, latch: bool = False) -> dict | None:
    if latch:
        matches = LATCH_RE.findall(text)
        if not matches:
            return None
        match = matches[-1]
        return {
            "latch_on_s": float(match[7]),
            "latch_end_s": float(match[8]),
            "latch_frames": int(match[2]),
            "latch_mean_ms": float(match[3]),
            "latch_p99_ms": float(match[4]),
            "latch_worst_ms": float(match[5]),
            "latch_slow": int(match[6]),
            "late_frames": int(match[10]),
            "late_mean_ms": float(match[11]),
            "late_p99_ms": float(match[12]),
            "late_worst_ms": float(match[13]),
            "late_slow": int(match[14]),
            "gens": int(match[9]),
            "budget_start": int(match[15]),
            "budget_end": int(match[16]),
            "backlog_end": int(match[17]),
            "reasons_end": int(match[18]),
            "visible_backlog_end": int(match[19]),
            "visible_full": int(match[20]),
            "visible_mid": int(match[21]),
            "visible_far": int(match[22]),
            "visible_band_0": int(match[23]),
            "visible_band_1": int(match[24]),
            "visible_band_2": int(match[25]),
            "visible_band_3": int(match[26]),
            "visible_band_4": int(match[27]),
            "visible_band_5": int(match[28]),
            "far_total": int(match[29]),
            "far_no_chunk": int(match[30]),
            "far_empty": int(match[31]),
            "far_no_geometry": int(match[32]),
            "far_solid": int(match[33]),
            "far_enclosed": int(match[34]),
            "far_hidden": int(match[35]),
            "far_drawn_chunk": int(match[36]),
            "far_drawn_region": int(match[37]),
            "far_hole_total": int(match[38]),
            "far_hole_visible": int(match[39]),
            "far_hole_occluded": int(match[40]),
            "far_screen_visible": int(match[41]),
            "far_screen_pixels": int(match[42]),
            "far_screen_coverage": float(match[43]),
        }
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
    settle_match = re.search(r"PROBE fill settle t=([0-9.]+)s", text)
    row = {
        "loaded_s": loaded,
        "mesh_s": float(match[1]),
        "far_s": float(match[2]),
        "end_s": end,
        "settle_s": float(settle_match.group(1)) if settle_match else -1.0,
        "tail_s": max(0.0, end - loaded),
        "samples": int(match[4]),
        "frames": int(match[5]),
        "worst_ms": float(match[6]),
        "slow": int(match[7]),
        "load_avg_ms": float(frame.group(1)),
        "process_total_s": float(process.group(1)),
        "chunk_load_s": -1.0,
        "first_upload_s": -1.0,
        "first_visible_s": -1.0,
        "visible_50_s": -1.0,
        "visible_90_s": -1.0,
        "visible_target": 0,
        "cache_open": 0, "cache_hits": 0, "cache_misses": 0, "cache_hit_rate": 0.0,
        "cache_packs": 0, "cache_entries": 0, "cache_staged": 0, "cache_written": 0,
        "cache_packs_written": 0, "cache_merged": 0, "cache_write_mb": 0.0,
        "cache_read_ms": 0.0, "cache_decode_ms": 0.0, "cache_encode_ms": 0.0,
        "cache_write_ms": 0.0, "cache_encode_failures": 0, "cache_write_failures": 0,
    }
    first = FIRST_RE.search(text)
    if first is not None:
        row.update({
            "chunk_load_s": float(first.group(1)),
            "first_upload_s": float(first.group(2)),
            "first_visible_s": float(first.group(3)),
            "visible_50_s": float(first.group(4)),
            "visible_90_s": float(first.group(5)),
            "visible_target": int(first.group(6)),
        })
    cache = CACHE_RE.search(text)
    if cache is not None:
        row.update({
            "cache_open": int(cache.group(1)),
            "cache_hits": int(cache.group(2)),
            "cache_misses": int(cache.group(3)),
            "cache_hit_rate": float(cache.group(4)),
            "cache_packs": int(cache.group(5)),
            "cache_entries": int(cache.group(6)),
            "cache_read_ms": float(cache.group(7)),
            "cache_decode_ms": float(cache.group(8)),
            "cache_staged": int(cache.group(9)),
            "cache_written": int(cache.group(10)),
            "cache_packs_written": int(cache.group(11)),
            "cache_merged": int(cache.group(12)),
            "cache_write_mb": float(cache.group(13)),
            "cache_encode_ms": float(cache.group(14)),
            "cache_write_ms": float(cache.group(15)),
            "cache_encode_failures": int(cache.group(16)),
            "cache_write_failures": int(cache.group(17)),
        })
    return row


def run_one(label: str, arm: str, out_dir: Path, timeout_s: int, base_env: dict,
            latch: bool = False, flight: int = 36, vsync: bool = False,
            tail_budget_ms: int | None = None,
            final_install_ms: int | None = None,
            workers: int | None = None,
            hold: bool = False,
            hold_partial: int | None = None,
            cache: bool = False,
            rd: int = 32,
            extra_env: dict | None = None) -> dict | None:
    env = dict(base_env)
    for key in CLEAR_ENV:
        env.pop(key, None)
    env["RD"] = str(rd)
    env["PROBE_TIMEOUT_SECS"] = str(timeout_s)
    if tail_budget_ms is not None:
        env["FARLANDS_LOADING_TAIL_BUDGET_MS"] = str(tail_budget_ms)
    if workers is not None:
        env["FARLANDS_WORKER_COUNT"] = str(workers)
    if vsync:
        env["VSYNC_ON"] = "1"
    if extra_env:
        env.update(extra_env)
    if latch:
        env["FARLANDS_LOADING_LATCH_QUIET_SEC"] = "1"
        env["FARLANDS_LOADING_LATCH_MAX_SEC"] = "30" if arm == "A" else "1000000000"
        env["LATCH_FLIGHT"] = str(flight)
        description = "latch 30 s cap" if arm == "A" else "latch 1 s quiet, no cap"
    elif hold_partial is not None:
        # Both arms keep the hold; only the tiny nearest-only budget differs.
        env["FARLANDS_HOLD_MESH_UNTIL_LOADED"] = "1"
        env["FARLANDS_HOLD_MESH_PARTIAL"] = "0" if arm == "A" else str(hold_partial)
        description = "hold, partial 0" if arm == "A" else f"hold, partial {hold_partial}"
    elif hold:
        env["FARLANDS_HOLD_MESH_UNTIL_LOADED"] = "1" if arm == "A" else "0"
        description = "initial mesh hold" if arm == "A" else "overlap, no hold"
    elif cache:
        if arm == "A":
            env["FARLANDS_TERRAIN_CACHE"] = "1"
            description = "cache on (prepared)"
        else:
            description = "cache off"
    else:
        if arm == "B":
            env["NO_RENDER"] = "1"
        description = "NO_RENDER=1" if arm == "B" else "render loop on"
    if final_install_ms is not None:
        # This is an engine-budget A/B, not the render/no-render pair: both arms
        # must render or the settle difference is mostly the renderer.
        env.pop("NO_RENDER", None)
        env["FARLANDS_FINAL_INSTALL_BUDGET_MS"] = "0.75" if arm == "A" else str(final_install_ms)
        description = ("render, final install 0.75 ms" if arm == "A"
                       else f"render, final install {final_install_ms} ms")

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
    row = parse_output(text, latch=latch)
    ok_marker = "PROBE OK" if latch else "PROBE fill OK"
    if code != 0 or row is None or ok_marker not in text:
        print(f"FAILED (rc={code}, {elapsed:.0f}s)")
        print("\n".join(text.splitlines()[-25:]))
        return None

    row["run_s"] = elapsed
    row["log"] = str(log_path)
    if latch:
        print(f"on={row['latch_on_s']:.2f}s end={row['latch_end_s']:.2f}s "
              f"mean={row['latch_mean_ms']:.2f}ms ({elapsed:.0f}s)")
    else:
        print(f"settle={row['settle_s']:.3f}s end={row['end_s']:.2f}s ({elapsed:.0f}s)")
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


def cache_directory() -> Path:
    """Godot's `user://` for this project on this machine."""
    return (Path(os.environ.get("APPDATA", "")) / "Godot" / "app_userdata" /
            "Farlands" / "chunks" / "cache")


def wipe_cache() -> None:
    shutil.rmtree(cache_directory(), ignore_errors=True)


def metric_block(lines: list[str], label: str, unit: str, digits: int,
                 a_rows: list[dict], b_rows: list[dict], key: str) -> None:
    a_med, a_lo, a_hi = median_range([float(row[key]) for row in a_rows])
    b_med, b_lo, b_hi = median_range([float(row[key]) for row in b_rows])
    lines.append(
        f"{label:<14} A {a_med:.{digits}f}{unit} [{a_lo:.{digits}f}-{a_hi:.{digits}f}]   "
        f"B {b_med:.{digits}f}{unit} [{b_lo:.{digits}f}-{b_hi:.{digits}f}]"
    )


def diff_block(lines: list[str], label: str, unit: str, digits: int,
               pairs: list[tuple[int, dict, dict]], key: str,
               better_label: str | None = "B faster") -> list[float]:
    diffs = [float(a[key]) - float(b[key]) for _, a, b in pairs]
    med, low, high = median_range(diffs)
    faster = sum(1 for value in diffs if value > 0.0)
    slower = sum(1 for value in diffs if value < 0.0)
    ties = len(diffs) - faster - slower
    detail = (f"   {better_label} {faster}/{len(diffs)} "
              f"(other {slower}, tie {ties})") if better_label else ""
    lines.append(
        f"{label:<14} A-B median {med:+.{digits}f}{unit} "
        f"[{low:+.{digits}f}-{high:+.{digits}f}]{detail}"
    )
    return diffs


def build_summary(args: argparse.Namespace, out_dir: Path, commit: str,
                  dirty: str, warmup: dict | None,
                  pairs: list[tuple[int, dict, dict]]) -> str:
    lines = [
        "Start-fill AB/BA harness",
        f"commit: {commit or 'unknown'}",
        f"tracked dirty: {dirty or 'clean'}",
        ("A: render, final install 0.75 ms" if args.final_install_ms is not None
         else "A: latch 30 s cap, 1 s quiet" if args.latch
         else "A: hold, partial 0" if args.hold_partial is not None
         else "A: initial mesh hold" if args.hold
         else "A: cache on, prepared" if args.cache
         else "A: normal render loop"),
        (f"B: render, final install {args.final_install_ms} ms" if args.final_install_ms is not None
         else "B: latch 1 s quiet, no cap" if args.latch
         else f"B: hold, partial {args.hold_partial}" if args.hold_partial is not None
         else "B: overlap, no hold" if args.hold
         else "B: cache off" if args.cache
         else "B: NO_RENDER=1"),
        f"pairs: {args.pairs}, arm order alternates per pair (AB, BA, AB, ...)",
        *( [f"flight: {args.flight}s"] if args.latch else [] ),
        f"timeout: {args.timeout}s per run",
        f"cores: {args.cores or 'all'}",
        f"vsync: {'on' if args.vsync else 'off'}",
        f"cache: {'A on, B off, rebuilt each pair' if args.cache else 'off'}",
        (        f"tail budget: {args.tail_budget_ms} ms" if args.tail_budget_ms is not None
         else "tail budget: build default"),
        (f"final install: A 0.75 ms, B {args.final_install_ms} ms"
         if args.final_install_ms is not None else "final install: build default"),
        f"output: {out_dir}",
        "",
    ]
    if warmup is not None:
        if args.latch:
            lines.append(
                f"warmup (discarded): on={float(warmup['latch_on_s']):.2f}s "
                f"end={float(warmup['latch_end_s']):.2f}s "
                f"mean={float(warmup['latch_mean_ms']):.2f}ms log={warmup['log']}"
            )
        else:
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
    if args.latch:
        metric_block(lines, "latch on", "s", 2, a_rows, b_rows, "latch_on_s")
        metric_block(lines, "latch end", "s", 2, a_rows, b_rows, "latch_end_s")
        metric_block(lines, "frames", "", 0, a_rows, b_rows, "latch_frames")
        metric_block(lines, "frame mean", "ms", 2, a_rows, b_rows, "latch_mean_ms")
        metric_block(lines, "frame p99", "ms", 2, a_rows, b_rows, "latch_p99_ms")
        metric_block(lines, "frame worst", "ms", 2, a_rows, b_rows, "latch_worst_ms")
        metric_block(lines, "slow frames", "", 0, a_rows, b_rows, "latch_slow")
        metric_block(lines, "late mean", "ms", 2, a_rows, b_rows, "late_mean_ms")
        metric_block(lines, "late p99", "ms", 2, a_rows, b_rows, "late_p99_ms")
        metric_block(lines, "late worst", "ms", 2, a_rows, b_rows, "late_worst_ms")
        metric_block(lines, "late slow", "", 0, a_rows, b_rows, "late_slow")
        metric_block(lines, "gens", "", 0, a_rows, b_rows, "gens")
        metric_block(lines, "budget end", "", 0, a_rows, b_rows, "budget_end")
        metric_block(lines, "backlog end", "", 0, a_rows, b_rows, "backlog_end")
        metric_block(lines, "visible end", "", 0, a_rows, b_rows, "visible_backlog_end")
        for label, keys in (
            ("visible tiers", ("visible_full", "visible_mid", "visible_far")),
            ("visible bands", tuple(f"visible_band_{i}" for i in range(6))),
            ("far geometry", ("far_total", "far_no_chunk", "far_empty",
                              "far_no_geometry", "far_solid", "far_enclosed",
                              "far_hidden", "far_drawn_chunk", "far_drawn_region")),
            ("far visible", ("far_hole_total", "far_hole_visible", "far_hole_occluded")),
        ):
            a_vals = [median_range([float(row[key]) for row in a_rows])[0] for key in keys]
            b_vals = [median_range([float(row[key]) for row in b_rows])[0] for key in keys]
            lines.append(
                f"{label:<14} A {'/'.join(f'{value:.0f}' for value in a_vals)}   "
                f"B {'/'.join(f'{value:.0f}' for value in b_vals)}"
            )
        a_screen = [median_range([float(row[key]) for row in a_rows])[0]
                    for key in ("far_screen_visible", "far_screen_pixels", "far_screen_coverage")]
        b_screen = [median_range([float(row[key]) for row in b_rows])[0]
                    for key in ("far_screen_visible", "far_screen_pixels", "far_screen_coverage")]
        lines.append(
            f"{'far screen':<14} A {a_screen[0]:.0f}/{a_screen[1]:.0f}/{a_screen[2]:.4f}   "
            f"B {b_screen[0]:.0f}/{b_screen[1]:.0f}/{b_screen[2]:.4f}"
        )
    else:
        metric_block(lines, "settle", "s", 3, a_rows, b_rows, "settle_s")
        metric_block(lines, "loaded", "s", 2, a_rows, b_rows, "loaded_s")
        metric_block(lines, "end", "s", 2, a_rows, b_rows, "end_s")
        metric_block(lines, "tail", "s", 2, a_rows, b_rows, "tail_s")
        metric_block(lines, "chunk load", "s", 2, a_rows, b_rows, "chunk_load_s")
        metric_block(lines, "first upload", "s", 2, a_rows, b_rows, "first_upload_s")
        metric_block(lines, "first visible", "s", 2, a_rows, b_rows, "first_visible_s")
        metric_block(lines, "visible 50%", "s", 2, a_rows, b_rows, "visible_50_s")
        metric_block(lines, "visible 90%", "s", 2, a_rows, b_rows, "visible_90_s")
        metric_block(lines, "frames", "", 0, a_rows, b_rows, "frames")
        metric_block(lines, "load_avg", "ms", 2, a_rows, b_rows, "load_avg_ms")
        metric_block(lines, "process_total", "s", 2, a_rows, b_rows, "process_total_s")
        metric_block(lines, "worst frame", "ms", 2, a_rows, b_rows, "worst_ms")
        metric_block(lines, "slow frames", "", 0, a_rows, b_rows, "slow")
        if args.cache:
            metric_block(lines, "cache hit rate", "", 3, a_rows, b_rows, "cache_hit_rate")
            metric_block(lines, "cache entries", "", 0, a_rows, b_rows, "cache_entries")
            metric_block(lines, "cache read", "ms", 1, a_rows, b_rows, "cache_read_ms")
            metric_block(lines, "cache decode", "ms", 1, a_rows, b_rows, "cache_decode_ms")

    if args.latch:
        lines += ["", "Paired differences (A-B):"]
        diff_block(lines, "latch on", "s", 2, pairs, "latch_on_s", better_label=None)
        diff_block(lines, "latch end", "s", 2, pairs, "latch_end_s", better_label=None)
        diff_block(lines, "frame mean", "ms", 2, pairs, "latch_mean_ms", better_label="B lower")
        diff_block(lines, "frame p99", "ms", 2, pairs, "latch_p99_ms", better_label="B lower")
        diff_block(lines, "late mean", "ms", 2, pairs, "late_mean_ms", better_label="B lower")
        diff_block(lines, "late p99", "ms", 2, pairs, "late_p99_ms", better_label="B lower")
        diff_block(lines, "backlog end", "", 0, pairs, "backlog_end", better_label=None)
        diff_block(lines, "visible end", "", 0, pairs, "visible_backlog_end", better_label=None)
    else:
        lines += ["", "Paired differences (positive = B loaded/finished sooner):"]
        diff_block(lines, "settle", "s", 3, pairs, "settle_s")
        diff_block(lines, "loaded", "s", 2, pairs, "loaded_s")
        diff_block(lines, "end", "s", 2, pairs, "end_s")
        diff_block(lines, "tail", "s", 2, pairs, "tail_s")

    lines += ["", "Pairs:"]
    for number, a, b in pairs:
        if args.latch:
            lines.append(
                f"  {number}  A on {float(a['latch_on_s']):.2f}s  B on {float(b['latch_on_s']):.2f}s  "
                f"A-B {float(a['latch_on_s']) - float(b['latch_on_s']):+.2f}s  "
                f"(A end {float(a['latch_end_s']):.2f}s, B end {float(b['latch_end_s']):.2f}s)"
            )
        else:
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
    parser.add_argument("--latch", action="store_true",
                        help="A/B the initial-loading latch: 30 s cap vs 1 s quiet with no cap")
    parser.add_argument("--hold", action="store_true",
                        help="A/B the initial mesh hold: on vs off (both render normally)")
    parser.add_argument("--hold-partial", type=int, default=None, metavar="N",
                        help="A/B the hold's nearest-only budget: A=0, B=N rebuilds/uploads per frame")
    parser.add_argument("--cache", action="store_true",
                        help="A/B the generated-terrain cache: A reads a prepared cache, B generates (both render)")
    parser.add_argument("--flight", type=int, default=36,
                        help="latch mode: seconds of 100 block/s flight (default 36)")
    parser.add_argument("--cores", type=int, default=0,
                        help="pin the harness and its Godot runs to the first N logical processors (Windows; 0 = all)")
    parser.add_argument("--workers", type=int, default=None,
                        help="override the worker pool size (default: N-1 when --cores is set; else the build's hardware_concurrency()-1)")
    parser.add_argument("--rd", type=int, default=32,
                        help="render distance for each run (default 32)")
    parser.add_argument("--fill-trace", action="store_true",
                        help="print loaded/generating/completed/pending/budget and fill-settled once a second")
    parser.add_argument("--vsync", action="store_true",
                        help="enable display vsync for every run (the constrained-hardware arm)")
    parser.add_argument("--tail-budget-ms", type=int, default=None,
                        help="cap the three post-load clocks at N ms in every run (default: build value)")
    parser.add_argument("--final-install-ms", type=int, default=None, metavar="N",
                        help="A/B the final-wave completion budget: A=0.75, B=N ms")
    args = parser.parse_args(argv)
    if args.pairs < 1:
        parser.error("--pairs must be at least 1")
    if args.latch and args.hold:
        parser.error("--latch and --hold cannot be combined")
    if args.hold_partial is not None:
        if args.latch or args.hold:
            parser.error("--hold-partial cannot be combined with --latch or --hold")
        if args.hold_partial < 1:
            parser.error("--hold-partial must be at least 1")
    if args.cache and (args.latch or args.hold or args.hold_partial is not None):
        parser.error("--cache cannot be combined with --latch, --hold or --hold-partial")
    if args.flight <= 6:
        parser.error("--flight must be greater than the 6 s late window")
    if args.cores < 0:
        parser.error("--cores cannot be negative")
    if args.rd < 1:
        parser.error("--rd must be at least 1")
    workers = args.workers
    if workers is None and args.cores > 0:
        workers = max(1, args.cores - 1)
    if workers is not None and workers < 1:
        parser.error("--workers must be at least 1")
    if args.tail_budget_ms is not None and args.tail_budget_ms <= 0:
        parser.error("--tail-budget-ms must be positive")
    if args.final_install_ms is not None and args.final_install_ms <= 0:
        parser.error("--final-install-ms must be positive")
    mask = pin_to_cores(args.cores)

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
        ("A: render, final install 0.75 ms" if args.final_install_ms is not None
         else "A: latch 30 s cap, 1 s quiet" if args.latch
         else "A: hold, partial 0" if args.hold_partial is not None
         else "A: initial mesh hold" if args.hold
         else "A: cache on, prepared" if args.cache
         else "A: normal render loop"),
        (f"B: render, final install {args.final_install_ms} ms" if args.final_install_ms is not None
         else "B: latch 1 s quiet, no cap" if args.latch
         else f"B: hold, partial {args.hold_partial}" if args.hold_partial is not None
         else "B: overlap, no hold" if args.hold
         else "B: cache off" if args.cache
         else "B: NO_RENDER=1"),
        f"pairs: {args.pairs}",
        *( [f"flight: {args.flight}s"] if args.latch else [] ),
        f"warmup: {'no' if args.no_warmup else 'yes'}",
        f"timeout: {args.timeout}s",
        f"rd: {args.rd}",
        f"cores: {args.cores or 'all'}" + (f" (mask 0x{mask:x})" if mask else ""),
        f"workers: {workers if workers is not None else 'build default'}",
        f"fill trace: {'on' if args.fill_trace else 'off'}",
        f"vsync: {'on' if args.vsync else 'off'}",
        f"cache: {'A on, B off, rebuilt each pair' if args.cache else 'off'}",
        (f"tail budget: {args.tail_budget_ms} ms" if args.tail_budget_ms is not None
         else "tail budget: build default"),
        (f"final install: A 0.75 ms, B {args.final_install_ms} ms"
         if args.final_install_ms is not None else "final install: build default"),
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

    if args.cache:
        wipe_cache()

    if not args.no_warmup:
        warmup = run_one("warmup", "A", out_dir, args.timeout, base_env,
                         latch=args.latch, flight=args.flight, vsync=args.vsync,
                         tail_budget_ms=args.tail_budget_ms,
                         final_install_ms=args.final_install_ms, workers=workers,
                         hold=args.hold, hold_partial=args.hold_partial,
                         cache=args.cache, rd=args.rd,
                         extra_env={"FILL_TRACE": "1"} if args.fill_trace else None)
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
        if args.cache and (number > 1 or args.no_warmup):
            wipe_cache()
            prepared = run_one(f"prep{number}", "A", out_dir, args.timeout, base_env,
                               vsync=args.vsync, tail_budget_ms=args.tail_budget_ms,
                               final_install_ms=args.final_install_ms,
                               workers=workers, rd=args.rd, cache=True,
                               extra_env={"FILL_TRACE": "1"} if args.fill_trace else None)
            if prepared is None:
                print(f"\nPair {number} preparation failed; completed pairs are in the summary.")
                summary = build_summary(args, out_dir, commit, dirty, warmup, pairs)
                (out_dir / "summary.txt").write_text(summary + "\n", encoding="utf-8")
                print("\n" + summary)
                return 1
            prepared.update(pair=0, order="prepare", arm="A", label=f"prep{number}")
            append_row(csv_path, prepared)
            print(f"prepared cache: {prepared['loaded_s']:.2f}s load / "
                  f"{prepared['end_s']:.2f}s end")
        pair_rows: dict[str, dict] = {}
        for arm in order:
            label = f"{arm.lower()}{number}"
            row = run_one(label, arm, out_dir, args.timeout, base_env,
                          latch=args.latch, flight=args.flight, vsync=args.vsync,
                          tail_budget_ms=args.tail_budget_ms,
                          final_install_ms=args.final_install_ms, workers=workers,
                          hold=args.hold, hold_partial=args.hold_partial,
                          cache=args.cache, rd=args.rd,
                          extra_env={"FILL_TRACE": "1"} if args.fill_trace else None)
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
