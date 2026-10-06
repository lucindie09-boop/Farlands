# Contributing to Farlands

## Building

```bash
# Standard build (debug)
scons -j8

# Run tests (664 cases / 362,128 assertions across the 98 .cpp files in tests/)
scons test -j8
./bin/run_tests.exe

# Benchmark with regression check
scons bench -j8
./bin/benchmark.exe --check benchmark_baseline.txt

# The two structural guards (both also run in CI)
scons sizecheck     # no .cpp/.hpp above 500 lines
scons portability   # no Windows-only shape outside #ifdef _WIN32
```

## Code Conventions

### Naming

- **`_locked` suffix**: Method acquires NO lock itself. Caller MUST already hold the appropriate
  exclusive lock. Uses `_fast` accessors only.
- **`_fast` suffix**: Accessor that reads/writes ChunkData without acquiring a shard lock. Only
  callable from `_locked` methods.
- **Auto-locking methods** (no suffix): Acquire their own shared lock internally.
  `get_chunk_data()`, `get_chunk_render_data()`, `mark_chunks_dirty_for_light()`,
  `queue_dirty_chunk()`.
- **Public wrapper pattern**: Acquire exclusive lock → call `_locked` → release lock → call
  auto-locking accessors for dirty-marking.

### Source Files

- **No `.cpp` or `.hpp` file above 500 lines** (`src/`, `tests/`, `tools/`). `scons sizecheck`
  fails with the offender list and warns at 480. Split along responsibilities, never by size:
  the original file stays the entry point for its subject, new pieces are `<name>_<topic>.cpp`
  beside it, a helper that two files need goes in `<name>_internal.hpp` (test fixtures in
  `tests/<name>_test_support.hpp`, in a named namespace with `inline` helpers, since a header
  does not inherit the includer's `using namespace`). Move the text verbatim first, then change
  it. The history and the traps are in `docs/file_size_plan.md`.
- A new `.cpp` is picked up by `SConstruct`'s glob with no edit — unless its file is named in
  `shared_sources`, the fuzz source lists or a tool's `Program()`.

- **Four static gates run on every push, and each catches a different class of mistake:**
  - `scons sizecheck` — the 500-line cap above.
  - `scons portability` — a Windows-only include, `#pragma`, intrinsic or Win32 type outside
    an `#ifdef _WIN32` region, or `[[...]]` written after a decl-specifier. MSVC accepts both
    shapes and GCC/clang reject them, and the Linux jobs run only in CI, so such a mistake
    otherwise sits in the tree looking green.
  - `scons docscheck` — a path the markdown cites that does not exist, a link into a heading
    that does not exist, a heading with no blank line above it, a stated count that no longer
    matches the tree, or a doc line/section too big to review. It scans `docs/` and the root
    `*.md`, so a new page is checked without registering it anywhere.
  - clang-tidy over all of `src/` (`bugprone-*`, `concurrency-*`, `performance-*`), where any
    finding in project sources fails the job. It runs on Linux, so Windows-only code is outside
    its reach by construction; reproduce it locally with `python tools/check_tidy.py`.

### Documentation

Where a fact belongs, so it does not get written four times and drift:

- **How it works and why** → [ARCHITECTURE.md](ARCHITECTURE.md), and for the subsystems big
  enough to have their own page, the [per-system documents](docs/README.md).
- **A task** (add a block, a biome, a command, a shader) → [docs/howto.md](docs/howto.md).
- **A field in a JSON file** → [docs/data-schemas.md](docs/data-schemas.md).
- **A test, a benchmark or a CI job** → [docs/testing.md](docs/testing.md).
- **A word** → [docs/glossary.md](docs/glossary.md).
- **An instrument for a symptom** → [docs/debugging.md](docs/debugging.md).
- **A claim only the real engine can prove** → a probe, documented in
  [docs/probes.md](docs/probes.md).
- **Something tried and rejected** → a row in
  [docs/decisions/README.md](docs/decisions/README.md).
- **Hard-won history and traps** → the `*-notes.md` page beside the subsystem in
  [docs/](docs/README.md), or [AGENTS.md](AGENTS.md) for anything that cuts across systems.

The index that says which is [docs/README.md](docs/README.md). Wrap prose at ~96 columns and run
`scons docscheck`; `scons reflow` rewraps a long line mechanically (and refuses if the text
would change). Prefer a number with a command that reproduces it over a number stated in prose —
the gate can only recount what it can see.

### Locking Rules

The chunk map uses 64 shards with `shared_mutex` per shard. Violations cause deadlocks that are
extremely difficult to reproduce.

1. **Single-chunk/block accessors** lock only their own shard.
2. **Batch methods** lock all relevant shards in **ascending shard order** to prevent deadlock.
3. **ChunkData writes** require `lock_all_exclusive()` or `lock_keys_exclusive()` for the
   relevant shards.
4. **`_locked` methods** assume the caller holds the lock. They must NOT call auto-locking
   methods (which would self-deadlock on the exclusive lock).
5. **Auto-locking methods** acquire their own shared locks. They MUST NOT be called while
   holding an exclusive lock (shared→exclusive upgrade is not supported by `std::shared_mutex`).
6. **BFS bounded reach**: Max light level 15 < chunk size 32, so a 3×3×3 neighborhood (27 keys)
   covers all BFS paths from a single seed chunk.
7. **Thread pool**: Always a single shared pool. Do not split into separate gen/mesh pools
   (tried, reverted — starved generation throughput ~7×).
8. **No recursive shared acquisition**: Never re-acquire a shard lock you already hold shared
   (shared→shared or shared→exclusive). Windows SRW locks block new shared acquisitions once a
   writer is queued on a shard, so the recursion deadlocks the moment a worker waits exclusive.
   Use `queue_dirty_chunk_fast()` instead of `queue_dirty_chunk()` inside `lock_keys` scopes,
   and `ShardLock::reset()` before re-acquiring a periodically-refreshed lock (e.g.
   `BlockEditor::raycast` re-locks `lock_all()` every 8 DDA steps).

### Thread Safety

- `std::atomic` for counters and flags. Use `std::memory_order_relaxed` unless ordering matters.
- `std::mutex` for narrow-scope protecting (`pending_light_removals_`). Lock, do minimal work,
  unlock — never hold across BFS calls.
- `MeshBuildTask::execute` holds a shared `lock_keys` over the center chunk + 26 neighbors for
  the whole data read — this pins them (erasure needs an exclusive lock on the shard) and
  serializes the build against exclusive writers that mutate block/light data mid-build. The
  center chunk also carries `pending_mesh_builds`, which `try_unload_chunk` checks before
  erasing.

### Block Definitions

`data/block_definitions.json` is the **single source of truth** for block properties, textures,
and emissive maps. Do not hardcode block properties in C++. To add a block:

1. Add entry to `data/block_definitions.json`
2. Add texture file to `textures/blocks/` (16×16 PNG)
3. The block ID is assigned automatically by `BlockRegistry::load_from_json()`

### Palette Storage

- 8 sections per chunk (16³ each)
- Uniform sections (all one block type) cost only a palette entry
- `memory_usage()` returns capacity-based byte count for regression benchmarks

### Persistence

- **Save format v3**:
  `[width:u32][height:u32][depth:u32][version:u32=3][crc32:u32][RLE body...]`
- **Atomic writes**: `.tmp` → backup existing to `.bak` → rename to target
- **CRC recovery**: On mismatch, try `.bak` fallback; delete if both corrupt
- Pure persistence logic lives in `src/core/` and is shared by tests and `ChunkWorld`: the
  inventory INVE byte format in `inventory.hpp`, and edit-map serialize/deserialize/apply in
  `edit_map.hpp`. Never re-implement these formats in tests — call the core functions. Godot
  file orchestration (chunk save/load, the metadata header, atomic writes, the `.bak` fallback)
  stays in `src/world/chunk_world_persistence.cpp`.

### Testing

- Tests use `doctest` v2.5.0, auto-discovered via `Glob("tests/*.cpp")` in SConstruct.
- New test files go in `tests/` — no registration needed.
- `TestShardMap` in `test_concurrency.cpp` mirrors `ChunkMap`'s 64-shard locking without Godot
  dependencies. Reuse this pattern for standalone concurrent tests.
- Block-dependent tests must call `BlockRegistry::get_instance().initialize_default_blocks()`
  first.
- `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` guards out Godot-dependent mesh types for fuzz/test
  targets.
- Fuzz source lists are explicit and **godot-free by construction**: a file is in one because a
  harness needs it, not because it is nearby, and a source there may not reach a loader or
  registry that sits behind that define — such a target links on no machine but the CI runner,
  so a link error there cannot be reproduced by a local build.

### Mesh Building

- `MeshBuilder` takes a center `ChunkData&` plus 26 optional neighbor `ChunkData*` pointers.
- Null neighbors are treated as air — this is correct for edge chunks.
- The build must run under a shared `lock_keys` over the center + 26 neighbors (see Thread
  Safety) — never build from raw neighbor pointers acquired and released earlier.
- LOD uses per-chunk stride/detail reduction (`lod_distance`/`lod_detail_level`), not chunk
  merging.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system design. Key invariants:

- Chunk size: 32×32×32, world height: 1024
- Camera3D child named "Camera3D" is the frustum source
- Light propagation: additive-only per-chunk, multi-chunk via `LightPropagator` + ChunkMap
- `data/block_definitions.json` replaces all scattered C++ block definitions

## Pull Requests

- All tests must pass (`scons test -j8 && ./bin/run_tests.exe`)- The static gates must be clean:
  `scons sizecheck`, `scons portability`, `scons docscheck`, and no new clang-tidy findings in
  `src/`
- New features should include tests in `tests/`
- Benchmark regression check should pass for performance-sensitive changes
- Lock ordering: always ascending shard order, never hold exclusive lock across auto-locking
  calls
