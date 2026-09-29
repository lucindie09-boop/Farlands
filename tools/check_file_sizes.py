#!/usr/bin/env python3
"""File-size guard: keeps the C++ tree free of mega files.

Every .cpp/.hpp under src/, tests/ and tools/ must stay at or below MAX_LINES.
The cap is what keeps a file readable in one sitting and a new responsibility in
a new file, so it is enforced rather than merely intended:

    python tools/check_file_sizes.py          # check, exit 1 on a violation
    python tools/check_file_sizes.py --list   # list the largest files and exit 0

`scons sizecheck` runs the first form, and CI runs it as well.
"""

import argparse
import os
import sys

MAX_LINES = 500
WARN_LINES = 480
ROOTS = ("src", "tests", "tools")
EXTS = (".cpp", ".hpp")


def is_test_path(path):
    parts = path.replace("\\", "/").lower().split("/")
    name = parts[-1]
    return "test" in parts or name.startswith("test_") or name.endswith("_test.cpp")


def collect(root_dir):
    """Every candidate source file, as (relative path, line count)."""
    found = []
    for root in ROOTS:
        base = os.path.join(root_dir, root)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            # Vendored code and build output are not ours to police.
            dirnames[:] = [d for d in dirnames if d not in (".git", "build", "gen", "bin")]
            for name in filenames:
                if not name.endswith(EXTS):
                    continue
                full = os.path.join(dirpath, name)
                try:
                    with open(full, "rb") as handle:
                        count = sum(1 for _ in handle)
                except OSError as exc:  # pragma: no cover - unreadable file
                    print(f"warning: cannot read {full}: {exc}", file=sys.stderr)
                    continue
                rel = os.path.relpath(full, root_dir)
                found.append((rel, count))
    found.sort(key=lambda item: item[1], reverse=True)
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="print the largest files and exit 0")
    parser.add_argument("--limit", type=int, default=25, help="rows to print with --list")
    args = parser.parse_args()

    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = collect(root_dir)

    if args.list:
        print(f"{len(files)} C++ files; cap is {MAX_LINES} lines")
        for rel, count in files[: args.limit]:
            flag = "OVER" if count > MAX_LINES else "    "
            print(f"  {flag} {count:6d}  {rel}")
        return 0

    over = [(rel, count) for rel, count in files if count > MAX_LINES]
    warn = [(rel, count) for rel, count in files if WARN_LINES < count <= MAX_LINES]

    for rel, count in warn:
        print(f"warning: {rel} is {count} lines (cap {MAX_LINES}, {MAX_LINES - count} left)")

    if not over:
        print(f"file sizes OK: {len(files)} C++ files, none above {MAX_LINES} lines")
        return 0

    print(f"file size check FAILED: {len(over)} file(s) above {MAX_LINES} lines", file=sys.stderr)
    for rel, count in over:
        print(f"  {count:6d}  {rel}  (+{count - MAX_LINES})", file=sys.stderr)
    print("\nSplit the file along its responsibilities (see .freebuff/file_size_plan.md).", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
