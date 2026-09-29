# Farlands documentation

Five documents answer five different questions. Read the one that matches yours
rather than the biggest one.

| Question | Document |
|---|---|
| What is this, how do I build it, what are the controls? | [README.md](../README.md) |
| How does the engine work, and why is it built this way? | [ARCHITECTURE.md](../ARCHITECTURE.md) |
| I want to change something — what do I have to touch? | [howto.md](howto.md) |
| What does this key in this JSON file do? | [data-schemas.md](data-schemas.md) |
| What does this word mean? | [glossary.md](glossary.md) |
| Something is wrong in game — where do I look? | [debugging.md](debugging.md) |
| How is a feature proved to work? | [probes.md](probes.md) |
| What has already been tried and rejected? | [decisions/](decisions/README.md) |
| How do I contribute without breaking the gates? | [CONTRIBUTING.md](../CONTRIBUTING.md) |
| Why is the tree split the way it is? | [file_size_plan.md](file_size_plan.md) |

[AGENTS.md](../AGENTS.md) is the engineering record: constraints, the history of
what each system cost to get right, and the traps that were paid for once. It is
long on purpose and organised by subsystem, not by question — start from a
section heading there, not from the top.

## The four gates

Every push runs four checks that exist because each one caught something a
normal build cannot see. All four are aliases, so they are one command each:

| Gate | Catches |
|---|---|
| `scons sizecheck` | a `.cpp`/`.hpp` over 500 lines ([tools/check_file_sizes.py](../tools/check_file_sizes.py)) |
| `scons portability` | a Windows-only shape outside `#ifdef _WIN32`, or a standard attribute after a decl-specifier ([tools/check_portability.py](../tools/check_portability.py)) |
| `scons docscheck` | a cited path that does not exist, a count that no longer matches the tree, a doc line nobody can diff ([tools/check_docs.py](../tools/check_docs.py)) |
| clang-tidy | a `bugprone-*`, `concurrency-*` or `performance-*` finding in `src/` — locally `python tools/check_tidy.py` |

The first three run in CI directly; the fourth is the static-analysis job. The
common thread is that all four are Linux-only in CI, so each one also exists as a
local command that reproduces it.

`docscheck` is the only one that reads outside the tree: once `bin/run_tests`
exists it runs it (once, cached) so that an assertion count in prose is compared
against the real number rather than only noted as unverified. `scons test` first
if you want that comparison; `CHECK_DOCS_NO_SUITE=1` to skip it.

## Adding to this directory

Write in the same voice as the rest: say what a thing does and why it is that way
rather than what it is called, wrap prose at ~96 columns, wrap code spans and
links as atoms, and run `scons docscheck` (plus `scons reflow` if a line got
long). The gate checks paths, counts and budgets; it cannot check that what you
wrote is true.
