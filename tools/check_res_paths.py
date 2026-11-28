#!/usr/bin/env python3
"""Fail when a `res://` path literal's case does not match the file on disk.

`res://Assets/shaders/voxel_shader.gdshader` resolves on Windows against
`assets/shaders/`, so a path written in the wrong case works on the machine it
was written on and fails in an export or on a case-sensitive checkout. The only
runtime feedback is the engine's `open_internal` warning, one line per path, so
this gate checks the same thing statically: each literal is looked up one
component at a time against the names the directory actually holds, and a
component that matches only case-insensitively is a failure.

Checked: every `res://` literal in scripts, scenes, resources, shaders, data,
C++, probes, tools and import metadata, resolved against the nearest
`project.godot` (so `probes/empty_project`'s `res://warm.gd` has its own root).
A missing path fails too: on a case-sensitive checkout a drift and a missing
file are the same thing.

Not checked: `res://.godot/...` (generated import cache), paths with a
placeholder (`%s`, `<name>`), and `.gdextension`, whose library entries name
artifacts for every platform rather than files this checkout has.

Usage:
    python tools/check_res_paths.py            # or: scons rescheck
    python tools/check_res_paths.py --selftest
"""

import argparse
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Directories that hold no source of ours, or nothing a clone has.
SKIP_DIRS = {
    ".git", ".godot", ".freebuff", "bin", "build", "godot-cpp", "MCPipeline",
    "node_modules", "screenshots",
}
# Extensions that can hold a `res://` literal. `.godot` is `project.godot`.
# `.gdextension` is deliberately absent: its entries name a library per
# platform, so only the current one's file exists in any checkout.
TEXT_EXTS = (
    ".gd", ".tscn", ".tres", ".gdshader", ".gdshaderinc", ".json", ".cfg",
    ".cpp", ".hpp", ".py", ".sh", ".import", ".godot",
)
# A literal with one of these is a pattern, not a path: `res://.../%s.png`,
# `res://.../<name>.png`, a `$PROBE` shell variable.
DYNAMIC = set("%{}*$")
# Inside a string a path may contain spaces (`Archive (Unused)/foo.png`), so the
# quote is the delimiter; outside one (a comment) whitespace ends it.
QUOTED = re.compile(r"""(["'])res://([^"'\n]+)\1""")
BARE = re.compile(r"res://([^\s\"'`)<>,;:]+)")


def iter_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        # Dot-directories are tool state (`.git`, `.godot`, `.cloud-attachments`),
        # never source: a conversation dump full of example paths is not a tree.
        dirnames[:] = sorted(d for d in dirnames
                             if d not in SKIP_DIRS and not d.startswith("."))
        for name in sorted(filenames):
            if name.lower().endswith(TEXT_EXTS):
                yield os.path.join(dirpath, name)


# This file quotes `res://` in its own docstring and selftest fixtures; those are
# examples of the check, not paths the project loads.
SELF = os.path.abspath(__file__)


def project_root_for(path, root):
    """The nearest ancestor with a `project.godot`, so a nested probe project
    resolves `res://` against itself rather than against the game."""
    d = os.path.dirname(os.path.abspath(path))
    root = os.path.abspath(root)
    while d != root and os.path.dirname(d) != d:
        if os.path.isfile(os.path.join(d, "project.godot")):
            return d
        d = os.path.dirname(d)
    return root


def resolve(base, rel):
    """(status, corrected_path): 'ok', 'case' or 'missing'.

    The lookup is exact per component, which is the whole point: `os.path.exists`
    on Windows is case-insensitive and would call a drifted path fine.
    """
    parts = [p for p in rel.split("/") if p not in ("", ".")]
    cur = base
    for i, part in enumerate(parts):
        try:
            names = os.listdir(cur)
        except OSError:
            return "missing", None
        if part not in names:
            folded = [n for n in names if n.lower() == part.lower()]
            if folded:
                return "case", "/".join(parts[:i] + [folded[0]] + parts[i + 1:])
            return "missing", None
        cur = os.path.join(cur, part)
    return ("ok" if os.path.exists(cur) else "missing"), None


def check(root):
    problems = []
    scanned = literals = 0
    for path in iter_files(root):
        if os.path.abspath(path) == SELF:
            continue
        try:
            data = open(path, "rb").read()
        except OSError:
            continue
        if b"\0" in data:  # a binary that slipped through the extension list
            continue
        scanned += 1
        text = data.decode("utf-8", "ignore")
        base = project_root_for(path, root)
        where = os.path.relpath(path, root).replace(os.sep, "/")

        def report(rel, offset):
            nonlocal literals
            rel = rel.rstrip(".,")
            if not rel or DYNAMIC & set(rel) or rel.startswith(".godot/"):
                return
            literals += 1
            status, stored = resolve(base, rel)
            if status == "ok":
                return
            line = text.count("\n", 0, offset) + 1
            problems.append((where, line, rel, status, stored))

        quoted = []
        for m in QUOTED.finditer(text):
            quoted.append((m.start(), m.end()))
            report(m.group(2), m.start(2))
        for m in BARE.finditer(text):
            if any(a <= m.start() < b for a, b in quoted):
                continue
            # `res://user://` in prose: the `:` after the match is the tell.
            if m.end() < len(text) and text[m.end()] == ":":
                continue
            report(m.group(1), m.start(1))
    return problems, scanned, literals


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "Assets", "shaders"))
        open(os.path.join(tmp, "project.godot"), "w").close()
        open(os.path.join(tmp, "Assets", "shaders", "thing.gdshader"), "w").close()
        good = os.path.join(tmp, "good.gd")
        bad = os.path.join(tmp, "bad.gd")
        gone = os.path.join(tmp, "gone.gd")
        open(good, "w").write('const A := preload("res://Assets/shaders/thing.gdshader")\n')
        problems, _, literals = check(tmp)
        if problems or literals != 1:
            print("selftest FAILED: the exact case should pass", file=sys.stderr)
            return 1
        open(bad, "w").write('const A := preload("res://assets/shaders/thing.gdshader")\n')
        problems, _, _ = check(tmp)
        if len(problems) != 1 or problems[0][3] != "case":
            print("selftest FAILED: a wrong-case path should be reported", file=sys.stderr)
            return 1
        open(gone, "w").write('const A := preload("res://Assets/shaders/gone.gdshader")\n')
        problems, _, _ = check(tmp)
        if len(problems) != 2 or not any(p[3] == "missing" for p in problems):
            print("selftest FAILED: a missing path should be reported", file=sys.stderr)
            return 1
    print("selftest OK: exact case passes; a case drift and a missing file both fail")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true",
                        help="prove the checks fire on a fixture and exit")
    args = parser.parse_args()
    if args.selftest:
        return selftest()

    problems, scanned, literals = check(ROOT)
    if not problems:
        print(f"res:// paths OK: {literals} literals in {scanned} files match the tree's case")
        return 0

    print(f"res:// path check FAILED: {len(problems)} literal(s) do not match the tree",
          file=sys.stderr)
    for where, line, rel, status, stored in problems:
        if status == "case":
            print(f"  {where}:{line}  res://{rel}  ->  on disk: res://{stored}",
                  file=sys.stderr)
        else:
            print(f"  {where}:{line}  res://{rel}  ->  no such file", file=sys.stderr)
    print("\nWrite the name the file has on disk; Windows hides the difference.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
