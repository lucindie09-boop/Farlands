# Farlands documentation

Read the document that matches your question rather than the biggest one. The two
files at the repository root are the map; the substance of a system is here.

## By question

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

## By system

A `*-notes.md` file is the engineering record of a system: what was decided, what
it cost, and the traps that were paid for once. The plain file beside it is the
description of how the system works today.

| System | Description | Notes |
|---|---|---|
| Block shapes, collision, liquids | [shapes.md](shapes.md) | [shapes-notes.md](shapes-notes.md) |
| Chunk streaming, LOD priority, shard locks | [streaming.md](streaming.md) | — |
| Far-field LOD modes (voxel tiers, seed-grid sampling) | — | [lod-modes.md](lod-modes.md) |
| Sky light, block light, what a shadow looks like | [light.md](light.md) | — |
| Rendering, liquids, the shader stack | — | [rendering-notes.md](rendering-notes.md) |
| Inventory, crafting, editors, viewmodel | — | [gui-notes.md](gui-notes.md) |
| GDScript UI scripts | [ui.md](ui.md) | — |
| Build files from other tools | — | [schematic-notes.md](schematic-notes.md) |
| Terrain generation and the worldgen data | [ARCHITECTURE.md](../ARCHITECTURE.md) | [terrain-notes.md](terrain-notes.md) |
| Pathfinding | — | [pathfinding.md](pathfinding.md) |
| Fluid flow | — | [fluids.md](fluids.md) |
| Tests, benchmarks and CI | [CONTRIBUTING.md](../CONTRIBUTING.md) | [testing.md](testing.md) |

[AGENTS.md](../AGENTS.md) holds the constraints that are not negotiable and the
notes that cut across systems (locking, the save format, the crash reporter, the
parts of code quality and performance that apply everywhere). Every heading in it
under *Major completed work* is now a summary of one of the pages above.

## The four gates

Every push runs four checks that exist because each one caught something a
normal build cannot see. All four are aliases, so they are one command each:

| Gate | Catches |
|---|---|
| `scons sizecheck` | a `.cpp`/`.hpp` over 500 lines ([tools/check_file_sizes.py](../tools/check_file_sizes.py)) |
| `scons portability` | a Windows-only shape outside `#ifdef _WIN32`, or a standard attribute after a decl-specifier ([tools/check_portability.py](../tools/check_portability.py)) |
| `scons docscheck` | a cited path that does not exist, a link into a heading that does not exist, a count that no longer matches the tree, a doc line nobody can diff ([tools/check_docs.py](../tools/check_docs.py)) |
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
long). Keep a section under the gate's word budget: if one grows past it, give it
subheadings or promote it to its own file rather than letting it become a wall.
The gate checks paths, anchors, counts and budgets; it cannot check that what you
wrote is true.
