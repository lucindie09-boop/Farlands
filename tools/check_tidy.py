#!/usr/bin/env python3
"""Reproduce CI's clang-tidy gate locally, over every `src/**/*.cpp`, in parallel.

The static-analysis job (`.github/workflows/build.yml`) runs clang-tidy over
`find src -name '*.cpp'` with a fixed check list, and a single finding in project
sources fails the job. That gate is Linux-only, so the two ways a local run
disagrees with it are worth knowing:

  * CI dedupes `compile_commands.json` by `.file` first, because SCons emits one
    entry per build target. This does the same.
  * CI never sees `clang-diagnostic-*` noise, because a Linux `compile_commands`
    carries GCC/clang flags. A database generated on Windows carries MSVC-only
    ones (`/c`, `/external:anglebrackets`), and clang then reports
    `clang-diagnostic-unused-command-line-argument` on nearly every file. Those
    are dropped here: the diagnostic namespace, not individual checks, since the
    artifact is a property of the local toolchain rather than of the code.

Windows-only code is outside this gate by construction (it is outside CI's too):
`crash_dump_report.cpp` is `#ifdef _WIN32` end to end, so the two
`performance-no-int-to-ptr` casts Win32 requires are suppressed in place with a
scoped NOLINT rather than restructured.

Usage:
    python tools/check_tidy.py                # every src/**/*.cpp (CI's set)
    python tools/check_tidy.py src/world/...  # only the files given

Exits 1 if any finding survives the filters, so it can gate a pre-push check.
"""

import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DB_DIR = os.path.join(ROOT, "build", "tidy_cc")
CHECKS = (
    "bugprone-*,concurrency-*,performance-*,"
    "-bugprone-easily-swappable-parameters,"
    "-bugprone-narrowing-conversions,"
    "-clang-analyzer-optin.core.EnumCastOutOfRange,"
    "-performance-move-const-arg"
)
FINDING = re.compile(r"warning:.*\[(?!clang-diagnostic-)")


def find_clang_tidy():
    """PATH first, then the usual LLVM install locations on Windows."""
    found = shutil.which("clang-tidy")
    if found:
        return found
    for guess in (
        r"C:\Program Files\LLVM\bin\clang-tidy.exe",
        "/usr/bin/clang-tidy",
        "/usr/local/opt/llvm/bin/clang-tidy",
    ):
        if os.path.exists(guess):
            return guess
    sys.exit("check_tidy: no clang-tidy found (install LLVM, or put it on PATH)")


def dedup_db():
    """CI's `jq 'unique_by(.file)'`: one entry per file, in a private copy."""
    path = os.path.join(ROOT, "compile_commands.json")
    if not os.path.exists(path):
        sys.exit("check_tidy: no compile_commands.json - run `scons` first")
    with open(path, encoding="utf-8") as fh:
        entries = json.load(fh)
    seen, unique = set(), []
    for entry in entries:
        key = entry["file"].replace("\\", "/").lower()
        if key in seen:
            continue
        seen.add(key)
        unique.append(entry)
    os.makedirs(DB_DIR, exist_ok=True)
    with open(os.path.join(DB_DIR, "compile_commands.json"), "w", encoding="utf-8") as fh:
        json.dump(unique, fh)
    return unique


def ci_file_list(entries):
    """`find src -name '*.cpp'`, resolved against the deduped database."""
    from_db = {
        e["file"].replace("\\", "/")
        for e in entries
        if e["file"].lower().endswith(".cpp")
    }
    on_disk = []
    for dirpath, _dirs, names in os.walk(os.path.join(ROOT, "src")):
        for name in names:
            if name.endswith(".cpp"):
                full = os.path.join(dirpath, name)
                on_disk.append(os.path.relpath(full, ROOT).replace("\\", "/"))
    return sorted(on_disk or from_db)


def run_one(tidy, rel_path):
    cmd = [tidy, os.path.join(ROOT, rel_path), "-p", DB_DIR,
           "--header-filter=src/", "-quiet", f"-checks={CHECKS}"]
    try:
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        return rel_path, ["TIMEOUT"]
    lines = [
        line.replace(ROOT + os.sep, "").replace("\\", "/")
        for line in (out.stdout + out.stderr).splitlines()
        if FINDING.search(line) and "godot-cpp" not in line
    ]
    return rel_path, lines


def main(argv):
    tidy = find_clang_tidy()
    entries = dedup_db()
    files = argv[1:] or ci_file_list(entries)
    if not files:
        sys.exit("check_tidy: no files to check")
    print(f"clang-tidy over {len(files)} file(s) [{tidy}]", flush=True)

    findings = {}
    workers = min(8, max(1, (os.cpu_count() or 2) - 1))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for rel_path, lines in pool.map(lambda f: run_one(tidy, f), files):
            if lines:
                findings[rel_path] = lines
                for line in lines:
                    print(line, flush=True)

    total = sum(len(v) for v in findings.values())
    print(f"\n=== {total} finding(s) in {len(findings)} file(s)", flush=True)
    if findings:
        print("CI's static-analysis job fails on any of these.", flush=True)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
