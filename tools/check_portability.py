#!/usr/bin/env python3
"""Catch the C++ that only one compiler accepts, before the other one sees it.

MSVC is the compiler this tree is developed against; GCC only sees it in CI, and
only after everything else has already passed.  Two shapes of mistake have
actually shipped that way, green on MSVC and fatal on GCC:

  * a standard attribute after a decl-specifier -- `inline [[nodiscard]] bool f()`
    is not valid C++ (clang: "standard attributes in middle of decl-specifiers"),
    while MSVC parses it happily;
  * Windows-only material outside an `#ifdef _WIN32` region -- a `dbghelp.h`
    include or a bare `HANDLE` reaches a Linux compiler that has neither.

Both are mechanical, so they are checked mechanically here rather than by
remembering.  Anything this reports is a build break on Linux or macOS; it is
not a style opinion.  Run it with `scons portability`, or directly:

    python tools/check_portability.py
    python tools/check_portability.py --selftest     # prove the checks fire
"""

import os
import re
import sys
import tempfile

ROOTS = ["src", "tests", "tools"]
EXTS = (".cpp", ".hpp")

# Headers that exist only on Windows, and the MSVC pragmas/extensions that no
# other compiler has.
WINDOWS_HEADERS = {
    "dbghelp.h", "tlhelp32.h", "windows.h", "io.h", "direct.h", "conio.h",
    "processthreadsapi.h", "synchapi.h", "winbase.h", "winnt.h", "shlobj.h",
    "psapi.h", "intrin.h", "excpt.h",
}
PRAGMA = re.compile(r"#\s*pragma\s+(?:comment|warning|optimize|intrinsic|auto_inline)\b")
INTRINSIC = re.compile(
    r"\b(?:"
    r"_countof|_vsnwprintf_s?|_snwprintf_s?|_snprintf_s?|_stricmp|_strnicmp|_wcsicmp|"
    r"_wfopen|_waccess|_mkdir|_rmdir|_chdir|_splitpath_s?|_fullpath|"
    r"__debugbreak|__assume|__forceinline|__declspec|__int64|__fastcall|"
    r"GetModuleFileNameW|GetModuleHandleExW|GetCurrentProcess|GetCurrentThread|"
    r"VirtualQuery|CreateFileW|CloseHandle|WriteFile|ReadFile|"
    r"StackWalk64|SymFromAddrW|SymGetLineFromAddrW64|SymFunctionTableAccess64|"
    r"SymGetModuleBase64|Module32FirstW|Module32NextW"
    r")\b"
)
# Win32 type names. Kept to spellings no project type uses, so a hit means a hit.
WINDOWS_TYPE = re.compile(
    r"\b(?:"
    r"HANDLE|HINSTANCE|HMODULE|HWND|HDC|HKEY|LPCWSTR|LPWSTR|LPCSTR|LPSTR|"
    r"DWORD|DWORD64|WORD|BYTE|UINT|ULONG|ULONG_PTR|LARGE_INTEGER|"
    r"EXCEPTION_POINTERS|EXCEPTION_RECORD|CONTEXT|STACKFRAME64|"
    r"MEMORY_BASIC_INFORMATION|SYMBOL_INFOW|IMAGE_DOS_HEADER|IMAGEHLP_LINEW64|"
    r"SECURITY_ATTRIBUTES|STARTUPINFO|PROCESS_INFORMATION|CRITICAL_SECTION"
    r")\b"
)
# `inline [[nodiscard]]`, `static [[nodiscard]]`, ... -- attribute must come
# first or follow the whole declaration.
ATTR_LATE = re.compile(r"\b(?:inline|static|constexpr|virtual|extern|const|mutable)\s+\[\[")

# A line belongs to the Windows side if enclosing `#if` conditions say so, or it
# opens inside `#ifndef _WIN32` (which is the other platform's branch: Windows
# code there is wrong for the same reason).
WIN_TOKEN = re.compile(r"\b(?:_WIN32|_WIN64|WIN32|_MSC_VER|_MSVC_LANG)\b")


def windows_condition(expr):
    """True if this preprocessor condition selects Windows-only code."""
    flat = expr.replace(" ", "")
    if "!defined(_WIN32)" in flat or "!defined(_MSC_VER)" in flat:
        return False
    if flat.startswith("!") and WIN_TOKEN.search(flat):
        return False
    return bool(WIN_TOKEN.search(flat))


def strip_comments(line, in_block):
    """Blank out comments and string literals, keeping the line's length."""
    out = []
    i = 0
    while i < len(line):
        if in_block:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out) + " " * (len(line) - i), True
            out.append(" " * (end + 2 - i))
            i = end + 2
            in_block = False
            continue
        two = line[i:i + 2]
        if two == "//":
            break
        if two == "/*":
            in_block = True
            i += 2
            continue
        if line[i] in "\"'":
            quote = line[i]
            out.append(quote)
            i += 1
            while i < len(line):
                if line[i] == "\\":
                    out.append(line[i:i + 2])
                    i += 2
                    continue
                out.append(line[i])
                if line[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(line[i])
        i += 1
    return "".join(out), in_block


def scan_text(path, text):
    """Yield (line_no, message) for every finding in one file's source."""
    findings = []
    guard = [False]          # enclosing `#if` chain: any Windows-positive level?
    in_block_comment = False
    lines = text.splitlines()
    for number, raw in enumerate(lines, 1):
        code, in_block_comment = strip_comments(raw, in_block_comment)
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", code)
        if directive:
            kind, expr = directive.group(1), directive.group(2)
            outer = guard[-2] if len(guard) > 1 else False
            if kind == "endif":
                if len(guard) > 1:
                    guard.pop()
            elif kind == "else":
                guard[-1] = outer
            elif kind == "elif":
                guard[-1] = outer or windows_condition(expr)
            else:
                positive = kind != "ifndef" and windows_condition(expr)
                guard.append(outer or positive)
            continue

        if guard[-1]:
            continue

        include = re.match(r'\s*#\s*include\s*[<"]([^>"]+)[>"]', code)
        if include and include.group(1).split("/")[-1].lower() in WINDOWS_HEADERS:
            findings.append((number, "Windows-only include outside an #ifdef _WIN32 region"))
        if PRAGMA.search(code):
            findings.append((number, "MSVC-only #pragma outside an #ifdef _WIN32 region"))
        for match in INTRINSIC.finditer(code):
            findings.append((number, "MSVC-only name '%s' outside an #ifdef _WIN32 region" % match.group(0)))
            break
        match = WINDOWS_TYPE.search(code)
        if match:
            findings.append((number, "Win32 type '%s' outside an #ifdef _WIN32 region" % match.group(0)))
        if ATTR_LATE.search(code):
            findings.append((number, "standard attribute must precede the decl-specifiers"))
    return findings


def walk():
    for root in ROOTS:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in (".git", "build")]
            for name in sorted(filenames):
                if name.endswith(EXTS):
                    yield os.path.join(dirpath, name)


def main(argv):
    if "--selftest" in argv:
        return selftest()
    total = 0
    for path in walk():
        with open(path, encoding="utf-8-sig", errors="replace") as handle:
            text = handle.read()
        findings = scan_text(path, text)
        if findings:
            total += len(findings)
            print(path)
            for number, message in findings:
                print("  %s:%d: %s" % (path, number, message))
    if total:
        print("portability: %d finding(s); these fail on GCC/clang" % total)
        return 1
    print("portability OK")
    return 0


def selftest():
    """The two mistakes that actually shipped, plus a guarded copy of each."""
    bad = (
        "#include <dbghelp.h>\n"
        "\n"
        "inline [[nodiscard]] bool write_dump(HANDLE file) {\n"
        "    return Module32NextW(nullptr);\n"
        "}\n"
        "\n"
        "#ifdef _WIN32\n"
        "#include <windows.h>\n"
        "static [[nodiscard]] int guarded(HANDLE file) { return 0; }\n"
        "#endif\n"
    )
    good = (
        "#ifdef _WIN32\n"
        "#include <dbghelp.h>\n"
        "\n"
        "[[nodiscard]] inline bool write_dump(HANDLE file) { return false; }\n"
        "#endif\n"
        "\n"
        "#if defined(_WIN32) || defined(_MSC_VER)\n"
        "#pragma comment(lib, \"dbghelp.lib\")\n"
        "[[nodiscard]] static inline int guarded(HANDLE file) { return 0; }\n"
        "#endif\n"
        "\n"
        "#ifndef _WIN32\n"
        "#include <unistd.h>\n"
        "#endif\n"
    )
    with tempfile.TemporaryDirectory() as tmp:
        bad_path = os.path.join(tmp, "bad.cpp")
        good_path = os.path.join(tmp, "good.cpp")
        with open(bad_path, "w", encoding="utf-8") as handle:
            handle.write(bad)
        with open(good_path, "w", encoding="utf-8") as handle:
            handle.write(good)
        bad_hits = scan_text(bad_path, bad)
        good_hits = scan_text(good_path, good)
    wanted = [
        "Windows-only include",
        "standard attribute",
        "MSVC-only name",
    ]
    flat = " | ".join(message for _, message in bad_hits)
    missing = [w for w in wanted if w not in flat]
    if missing or good_hits:
        print("selftest FAILED: missing %s, unexpected %s" % (missing, good_hits))
        return 1
    print("selftest OK: %d finding(s) in the broken copy, 0 in the guarded copy" % len(bad_hits))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
