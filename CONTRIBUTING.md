# Contributing to Farlands

## Building

```bash
# Standard build (debug)
python -m SCons -j8

# Run tests (182 tests / 157k+ assertions)
python -m SCons test -j8
.\bin\run_tests.exe

# Benchmark with regression check
python -m SCons bench -j8
.\bin\benchmark.exe --check benchmark_baseline.txt
```

## Code Conventions

### Naming

- **`_locked` suffix**: Method acquires NO lock itself. Caller MUST already hold the appropriate exclusive lock. Uses `_fast` accessors only.
- **`_fast` suffix**: Accessor that reads/writes ChunkData without acquiring a shard lock. Only callable from `_locked` methods.
- **Auto-locking methods** (no suffix): Acquire their own shared lock internally. `get_chunk_data()`, `get_chunk_render_data()`, `mark_chunks_dirty_for_light()`, `queue_dirty_chunk()`.
- **Public wrapper pattern**: Acquire exclusive lock → call `_locked` → release lock → call auto-locking accessors for dirty-marking.

### Locking Rules

The chunk map uses 64 shards with `shared_mutex` per shard. Violations cause deadlocks that are extremely difficult to reproduce.

1. **Single-chunk/block accessors** lock only their own shard.
2. **Batch methods** lock all relevant shards in **ascending shard order** to prevent deadlock.
3. **ChunkData writes** require `lock_all_exclusive()` or `lock_keys_exclusive()` for the relevant shards.
4. **`_locked` methods** assume the caller holds the lock. They must NOT call auto-locking methods (which would self-deadlock on the exclusive lock).
5. **Auto-locking methods** acquire their own shared locks. They MUST NOT be called while holding an exclusive lock (shared→exclusive upgrade is not supported by `std::shared_mutex`).
6. **BFS bounded reach**: Max light level 15 < chunk size 32, so a 3×3×3 neighborhood (27 keys) covers all BFS paths from a single seed chunk.
7. **Thread pool**: Always a single shared pool. Do not split into separate gen/mesh pools (tried, reverted — starved generation throughput ~7×).
8. **No recursive shared acquisition**: Never re-acquire a shard lock you already hold shared (shared→shared or shared→exclusive). Windows SRW locks block new shared acquisitions once a writer is queued on a shard, so the recursion deadlocks the moment a worker waits exclusive. Use `queue_dirty_chunk_fast()` instead of `queue_dirty_chunk()` inside `lock_keys` scopes, and `ShardLock::reset()` before re-acquiring a periodically-refreshed lock (e.g. `BlockEditor::raycast` re-locks `lock_all()` every 8 DDA steps).

### Thread Safety

- `std::atomic` for counters and flags. Use `std::memory_order_relaxed` unless ordering matters.
- `std::mutex` for narrow-scope protecting (`pending_light_removals_`). Lock, do minimal work, unlock — never hold across BFS calls.
- `MeshBuildTask::execute` holds a shared `lock_keys` over the center chunk + 26 neighbors for the whole data read — this pins them (erasure needs an exclusive lock on the shard) and serializes the build against exclusive writers that mutate block/light data mid-build. The center chunk also carries `pending_mesh_builds`, which `try_unload_chunk` checks before erasing.

### Block Definitions

`data/block_definitions.json` is the **single source of truth** for block properties, textures, and emissive maps. Do not hardcode block properties in C++. To add a block:

1. Add entry to `data/block_definitions.json`
2. Add texture file to `textures/blocks/` (16×16 PNG)
3. The block ID is assigned automatically by `BlockRegistry::load_from_json()`

### Palette Storage

- 8 sections per chunk (16³ each)
- Uniform sections (all one block type) cost only a palette entry
- `memory_usage()` returns capacity-based byte count for regression benchmarks

### Persistence

- **Save format v3**: `[width:u32][height:u32][depth:u32][version:u32=3][crc32:u32][RLE body...]`
- **Atomic writes**: `.tmp` → backup existing to `.bak` → rename to target
- **CRC recovery**: On mismatch, try `.bak` fallback; delete if both corrupt
- Pure decode logic lives in `src/core/chunk_persistence.hpp` (testable standalone). Godot file orchestration stays in `chunk_world.cpp`.

### Testing

- Tests use `doctest` v2.5.0, auto-discovered via `Glob("tests/*.cpp")` in SConstruct.
- New test files go in `tests/` — no registration needed.
- `TestShardMap` in `test_concurrency.cpp` mirrors `ChunkMap`'s 64-shard locking without Godot dependencies. Reuse this pattern for standalone concurrent tests.
- Block-dependent tests must call `BlockRegistry::get_instance().initialize_default_blocks()` first.
- `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` guards out Godot-dependent mesh types for fuzz/test targets.

### Mesh Building

- `MeshBuilder` takes a center `ChunkData&` plus 26 optional neighbor `ChunkData*` pointers.
- Null neighbors are treated as air — this is correct for edge chunks.
- The build must run under a shared `lock_keys` over the center + 26 neighbors (see Thread Safety) — never build from raw neighbor pointers acquired and released earlier.
- LOD uses per-chunk stride/detail reduction (`lod_distance`/`lod_detail_level`), not chunk merging.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full system design. Key invariants:

- Chunk size: 32×32×32, world height: 1024
- Camera3D child named "Camera3D" is the frustum source
- Light propagation: additive-only per-chunk, multi-chunk via `LightPropagator` + ChunkMap
- `data/block_definitions.json` replaces all scattered C++ block definitions

## Pull Requests

- All tests must pass (`python -m SCons test -j8 && .\bin\run_tests.exe`)
- New features should include tests in `tests/`
- Benchmark regression check should pass for performance-sensitive changes
- Lock ordering: always ascending shard order, never hold exclusive lock across auto-locking calls
