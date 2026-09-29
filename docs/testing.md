# Testing and CI

How a change is checked here: the suite's rules, what counts as measurement, and the
jobs that run on every push.

## The suite

- **588 test cases / 346,456 assertions**, declared across 88 of the 90 `.cpp` files in `tests/`
  (`test_main.cpp` is the doctest entry point, `mesh_manager_stub.cpp` supplies link stubs, and
  both are picked up by the same `Glob("tests/*.cpp")`)
- **Use `CHECK` / `CHECK_FALSE`, never `REQUIRE` or the `*_MESSAGE` forms**: this build compiles
  doctest with exceptions off, so `REQUIRE`/`REQUIRE_FALSE` expand to a
  `static_assert(false, "Exceptions are disabled!")` — a compile error whose text names the
  doctest header rather than your test — and `INFO`/`CAPTURE`/`CHECK_MESSAGE` become
  `static_cast<void>(0)` no-ops, so a "diagnostic" silently disappears. To print context on
  failure, do it yourself (`.freebuff`-style printf, or `tests/test_fluid_rules.cpp`'s
  `expect()` helper), or assert on a comparison doctest can stringify
  (`CHECK(problem == std::string())` prints both strings)
- **Measure the alternative before shipping a "wasteful but obvious" fix, and be ready for it to
  be neither.** The frustum pass was 95% of every check in the generation sweep, with 99.2% of
  its in-frustum candidates already loaded — and gating its re-arm on a view change
  (implemented, tested, benchmarked) removed its idle cost exactly while costing 8-10% of
  streaming throughput, so it was reverted with the numbers recorded in ARCHITECTURE.md. Two
  habits made that answerable in one afternoon: the benchmark A/B'd both versions on ONE build,
  and the losing measurement was worth as much as the winning one. A large share of checks is
  not a large cost; check the wall-clock column before acting on a ratio
- **A generation chain beat a frustum priority — "spread from what exists" over "what the camera
  sees"**: chunks queue their 4 horizontal neighbours on install, and the queue is drained
  through the SAME filters and budget as the ring walk (a priority that reorders, never
  bypasses). Measured: +5% throughput, -30% cold start, -35% sweep time vs the frustum pass,
  holes still 0. Two traps, each a real failure: read the chunk's emptiness BEFORE its data is
  moved into the map (after the move the pointer is null), and DROP a refused offer instead of
  re-queueing it — the queue refills faster than any budget drains, so re-queueing inflated 174k
  offers to 13.3M in one flight

## Measurement rules

- **Move work off the frame before you make it cheaper, and prove the mover works with a
  plan/claim split**: `src/world/column_prefetch.hpp` hands the sweep's per-column content
  bounds to pool workers. The two properties that make it safe are worth copying: the consumer
  falls back to computing the value itself when no answer has landed (so a lagging producer is
  slower, never wrong, and can never stall the frontier it feeds), and answers are validated by
  an EPOCH rather than by trusting the producer (a task that was already running when the
  terrain changed is refused instead of cached, and its request is retired anyway — a key left
  looking in flight is a column the prefetch silently stops covering).
  `tests/test_column_prefetch.cpp` pins both, plus the request dedupe and the over-cap drop. Two
  measurements decided the details: the request scan must be independent of the consumer's
  cursor AND cover the whole list on its first pass (a slice-per-frame scan left 39% of entered
  columns derived on the main thread), and the win is real but PARTIAL — 75-79% of the band
  time, not all of it, because the resident lookups in a band read are the other half
- **A per-frame budget bounds WORK, not time. Check which one blew before blaming the budget.**
  The band frontier checks its 2 ms budget after every column, so a frame can only overshoot by
  ONE column's cost — which made a 24 ms band frame read as impossible until `max_band_columns`
  (1 column) and `max_band_column_ms` (24.25 ms) were recorded alongside it. The same run's
  `max_cold_bounds_ms` was 2.38 ms, so the stall was not the 181 us range computation at all: it
  was inside one column's read (locked lookups, a cache insert) or this thread simply being
  descheduled with 15 workers on its cores. Record the count AND the slowest unit together
  whenever a budget is checked per item, or the two causes are indistinguishable
- **When a per-item budget is blown, count HOW MANY items are aware of the shape of the fix**:
  the sweep's band read is one budgeted unit per column, and the counters (`max_band_columns`,
  `max_band_column_ms`, and the bounds/resident/lock split recorded together) said a fat band
  frame is ONE column that stalled, not thousands that ran past the budget — which ruled out the
  budget and pointed at a single `ChunkMap::contains` acquisition. Chasing that one number found
  the real structure: the resident check is `count(band)` chunk lookups and each was its own
  shard lock on its own shard, so a column spanned up to 16 of them. Sharding BY COLUMN (mask
  the y field out of the hash) turned the whole check into one acquisition plus lock-free
  probes, and shrank every other multi-key lock on the way (a 3x3x3 is 9 columns, so 27-key
  exclusive passes take 9 shards, not 27). A/B'd on one build with a temporary env toggle in
  `key_to_shard`, then the toggle deleted: +4% throughput, -20% sweep time, -32% worst sweep
  frame, -38% cold start. **Ship the toggle only long enough to measure it** — it is what makes
  an A/B possible without a rebuild between arms, and leaving it in a header called from
  everywhere is not an option
- **A probe that lasts more than a few frames should be a benchmark, not a pass/fail
  assertion**: `probes/probe_stream_bench.gd` measures the two things a player feels — time
  from a 2048-block teleport until the 3x3 columns around you have ground (~200 ms), and columns
  still without ground in a 5x5 square while flying at 100 blocks/s for 40 s (0 holes) — at a
  fixed 60 fps so the engine's time budgets mean what they mean in the game, and what a STANDING
  STILL world costs (three phases: cold starts, a 40 s flight, then 10 s of nothing). Run it
  before and after any change to generation, streaming or the sweep; the numbers it prints
  (`fly_gen_chunks_per_s`, `sweep_split_ms`, `fly_frustum_checks`, `walk_refused`) are what
  ARCHITECTURE.md's sweep bullets quote. Two ways it lies if written carelessly: a hop shorter
  than the render distance lands inside the already-generated disc and measures nothing, and a
  column cached as "empty" must be re-scanned or a cold region reads as empty forever
- **A probe that edits the world is writing persisted edits — snapshot the world first**:
  `BlockEditor::add_block_edit` writes the edit map, which autosaves into `user://chunks/*.edit`
  (every 5 s), so a probe that places blocks changes the world the user plays even if the probe
  fails or is killed. Run those through `probes/run_probe.sh <probe.gd>`, which copies that
  directory aside, runs the probe, and restores it afterwards (and refuses to run while a GAME
  process is up, since it would save over the restore; the editor is fine). The first
  `probe_flow.gd` runs did not, and left stone shelves and a creep of poured water in the real
  world — `probes/clean_flow_probe_litter.gd` is the one-shot repair, written to remove only
  dynamic fluid and floating stone
- **A probe that edits the world must verify the write**: `BlockEditor::place_block` only
  APPLIES an edit when the target chunk has render data (`get_chunk_render_data_fast`);
  otherwise it queues a pending placement and returns, so the block never changes and a later
  read shows the terrain that was already there — which reads exactly like a wrong drop/mining
  rule. An all-rock chunk can sit in that state thanks to underground mesh culling, so a probe
  that spawned inside rock silently aims at the wrong block. Move the player to open ground and
  read the edit back before trusting it, and do NOT call `generate_chunk()` on an already-loaded
  chunk as a shortcut: that leaves it without render data and queues every later write to it
  (`_wait_for_applied_writes` in `probes/probe_hammer.gd` is the pattern)

## The CI jobs

- **Cross-platform CI**: 5-leg matrix (ubuntu plain/TSan/ASan+UBSan, macos plain, windows plain)
  plus fuzz, static-analysis, and coverage jobs. Every job that builds first runs
  `tools/check_file_sizes.py` (500-line cap, warning at 480), the build job also runs
  `tools/check_portability.py`, and the static-analysis job is clang-tidy (the constraint
  above). The Linux legs are the only place GCC/clang and the sanitizers ever see this code,
  which is what makes those three checks a class of their own
- **Concurrency tests**: 27 tests for shard locking, deadlock prevention, PaletteStorage,
  cross-chunk writers, and thread-pool work stealing
- **Integration soak tests**: Concurrent pipeline simulation with Phase 4 unload to exercise
  unload vs active work races
- **Benchmark tool**: 5 hot paths with `--check` regression detection mode
- **Fuzzing**: 5 libFuzzer harnesses for palette, chunk load, chunk recovery, light propagation,
  and mesh builder
- **Code coverage**: lcov coverage with integration tests and memory benchmark
