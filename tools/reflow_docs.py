#!/usr/bin/env python3
"""Rewrap long prose lines in the markdown docs, proving the text is unchanged.

A bullet written as one 35,000-character line cannot be reviewed: a one-word fix
diffs the whole paragraph. This wraps such a line into indented continuation
lines, which markdown renders identically.

Three rules keep it honest:

  * A code span or a link is an ATOM - spaces inside it are temporarily replaced
    with a sentinel, so wrapping can never break `scons -j14` or a long URL across
    lines.
  * Fenced code blocks, table rows, headings and already-short lines are left
    alone. (A table row cannot be wrapped at all, which is why the gate treats
    tables separately.)
  * The file is only written when collapsing all whitespace runs to a single
    space gives the SAME string before and after: the change is whitespace only,
    or it does not happen.

    python tools/reflow_docs.py [width] FILE...   # or: scons reflow
"""

import os
import re
import sys

ATOM = re.compile(r"(`[^`\n]*`|\[[^\]]*\]\([^)\n]*\)|https?://\S+)")
FENCE = re.compile(r"^\s*(```|~~~)")
LIST = re.compile(r"^(\s*(?:[-*+]|\d+[.)])\s+)")
SENTINEL = "\x00"


def protect(text):
    """Spaces inside atoms become a sentinel, so split() cannot break them."""
    return ATOM.sub(lambda m: m.group(0).replace(" ", SENTINEL), text)


def wrap_words(words, width):
    lines, cur = [], ""
    for word in words:
        if not cur:
            cur = word
        elif len(cur) + 1 + len(word) <= width:
            cur += " " + word
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def reflow_line(line, width):
    """Return the wrapping of one line, or None when it must be left alone."""
    if len(line) <= width or not line.strip():
        return None
    body = line.rstrip()
    marker = LIST.match(body)
    if marker:
        prefix, content = marker.group(1), body[marker.end():]
    else:
        stripped = body.lstrip()
        if not stripped:
            return None
        prefix, content = body[: len(body) - len(stripped)], stripped
    words = protect(content).split()
    if not words:
        return None
    pieces = wrap_words(words, max(20, width - len(prefix)))
    out = [prefix + pieces[0]]
    indent = " " * len(prefix)
    out.extend(indent + piece for piece in pieces[1:])
    return [part.replace(SENTINEL, " ") for part in out]


def process(path, width):
    with open(path, encoding="utf-8") as fh:
        original = fh.read()
    newline = "\r\n" if "\r\n" in original else "\n"
    lines = original.replace("\r\n", "\n").split("\n")

    out, in_fence, wrapped = [], False, 0
    for line in lines:
        if FENCE.match(line):
            in_fence = not in_fence
            out.append(line)
            continue
        if in_fence or line.lstrip().startswith("|") or re.match(r"^\s*#{1,6}\s", line):
            out.append(line)
            continue
        rewritten = reflow_line(line, width)
        if rewritten is None:
            out.append(line)
        else:
            out.extend(rewritten)
            wrapped += 1

    result = newline.join(out)
    if re.sub(r"\s+", " ", original) != re.sub(r"\s+", " ", result):
        print(f"  {path}: REFUSED - content would change")
        return 0
    if result == original:
        return 0
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(result)
    return wrapped


def main(argv):
    width = int(argv[1]) if len(argv) > 1 and argv[1].isdigit() else 96
    paths = argv[2:] if len(argv) > 2 and argv[1].isdigit() else argv[1:]
    if not paths:
        # `scons reflow`: every markdown file the docs gate scans, plus the plan.
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from check_docs import doc_files

        paths = doc_files() + ["docs/file_size_plan.md"]
    total = 0
    for path in paths:
        count = process(path, width)
        total += count
        if count:
            print(f"  {path}: rewrapped {count} line(s)")
    print(f"rewrapped {total} line(s) at width {width}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
