# Debugging

Work outwards from the cheapest instrument. Most engine problems announce
themselves in a counter before they are visible, and every instrument below is
already in the build.

## 1. The performance report

`debug_enabled` on the `ChunkManager` node prints a report every
`debug_print_interval` seconds. Its counters are grouped by the stage they
belong to, and four of them are invariants — **these must be 0**:

| Counter | Must be | Means |
|---|---|---|
| `Water-only skips` | 0 | An upload was skipped because the hash saw only the opaque mesh change. Water you can swim in and not see |
| `Unrendered` | 0 | Geometry in range, not covered by a far region, and nothing drawing it |
| `Liquid missing` | 0 | A chunk whose data holds liquid and whose opaque geometry is on the GPU, with no liquid geometry on the GPU |
| `Liquid repairs` | — | Remeshes spent fixing `Liquid missing`. Should not climb once the world is settled |

The report also carries the phase timings (`dirty_mesh_queue` and the band
timings among them) and the shard-lock contention readings
(`ChunkMap::drain_shard_lock_stats`): contended waits only, plus hold times for
multi-shard exclusive locks. Read the **ratio** — waits far larger than any hold
means a convoy behind many writers, not one slow lock.

## 2. `/genstats` — the generation sweep

`/genstats` prints what the sweep examined and why each candidate was rejected,
per pass, plus a rolling window over the last 120 frames; `/genstats reset`
zeroes the counters and keeps the candidate-list size. The reading that matters
is **generations per check**, and the state to recognise is a full check budget
with zero generations (the sweep is walking terrain that is already loaded).

`reject_loaded` high means near rings being re-confirmed; `reject_above` /
`reject_below` / `reject_oob` should be near zero, because the list is per-column
bands now. Holes in the world are the thing this instrument exists for: run
`probes/probe_stream_bench.gd` for the two numbers a player feels (cold start
after a teleport, and columns without ground during a flight).

The band's **shape** and what its ±32-block pad **bought** are two more readings.
`band shape` histograms `count(band)` over the 32 world slices for non-fill
columns (`fill columns` are reported separately, because their band reaches the
world floor by design); `band_max_slices` is the tallest band seen. The `installs`
lines classify every installed chunk against its column's estimated content
bounds: `above column top` and `below land` can only be offered because of the pad
(or the fill rule), so `above_empty` / `below_empty` count generations the pad
paid for and got nothing, while `inside [land, top]` is terrain the estimate
itself claimed and `bounds unknown` is installs with no cached bounds to judge
against. A fat 24-31 bucket with a large pad-empty count is a pad worth
narrowing; a small top bucket with few pad-empties says the ±32 is not the cost.

## 3. Crash reports

The handler is ours, installed before any class registers. It writes
`user://crashes/crash-<stamp>.txt` (exception code, read/write and address, the
faulting instruction's module and offset, a symbolised stack, the loaded modules,
our DLL's build time) and a `.dmp` beside it, plus a `-firstchance.txt` capped at
8 writes. The report is written **before** the dump because it is what can be
read without a debugger.

- `probes/run_crash_probe.sh` arms a deliberate fault and proves the whole
  path works in the build you have. It is debug-build and env-armed only
  (`FARLANDS_CRASH_TEST=1`), so a synthetic crash never lands in your folder
  unasked.
- A process that dies **before** the engine can print anything leaves a log that
  stops mid-initialisation, with no Godot error above it. That is the signature
  this handler was written for: read the stack first, then the module list, and
  only then reach for a debugger.

## 4. Probes

For anything that only exists on screen or only exists in the loaded registry, a
probe is the instrument — see [probes.md](probes.md). Two rules matter when
reading one:

- A probe that edits the world must go through `probes/run_probe.sh`, which
  snapshots and restores `user://chunks`. Both runners refuse to start while a
  game process is up, and that check exists because the earlier version deleted
  and recreated a live userdata directory.
- A probe that edits the world must **read the write back**. A write to a chunk
  with no render data is queued as a pending placement and returns without
  changing anything, which reads exactly like a wrong rule.

## 5. The benchmark

`./bin/benchmark.exe --check benchmark_baseline.txt` is the regression
instrument: six metrics, each with 2× headroom. Two traps in using it —
**run it on an idle machine**, never straight after a 14-way parallel build (a
first reading has reported `build_mesh` at 3.97 ms against a 3.80 ms baseline
that four idle runs put at 3.22–3.54), and remember it is *noise-sensitive in the
fast direction*, so a large improvement is worth re-running before believing too.

## Symptom → first instrument

| Symptom | Look at |
|---|---|
| Water visible but not drawable, or vice versa | The four invariants above, then the upload hash. Water needs both surfaces uploaded under one hash |
| A shape renders as a plain cube | The `shape` name did not resolve. Read `get_selection_boxes(id)` back — an unknown shape only logs |
| A block write "did nothing" | Pending placement: the chunk had no render data. Move to open ground and read it back |
| A chunk off-screen is never meshed | The view-first heap reserve. With one heap a merely off-screen chunk never reached the front |
| Chunks briefly draw at the wrong detail at a tier edge | The transition shell walk (`for_each_shell_cell`) versus the box filter it replaced |
| A large paste hangs | A locking read inside the exclusive 3×3×3 band. In a debug build the lock-order checker names the accessor and the shard |
| An ocean drains into a hole, or a build leaves a lake-shaped gap | Generated water is deliberately inert; a *pasted* liquid's `still` form is what keeps a lake in place |
| A pasted build is partly placed | The writer handed cells back because their chunk was not resident. The controller retries them; request/pin is the mechanism |
| Placing an item does nothing | The item has no `place` object. That is the only bridge into block space |
| The world is darker/different after a terrain change | `weirdness_scale` / `shape_strength_*` in `data/terrain_config.json`, then the biome amplification knobs, which blend across borders |
| A doc claim no longer holds | `scons docscheck` recounts the tree instead of you |

## Before you believe a change is good

```bash
scons -j14 && scons test -j14 && ./bin/run_tests.exe   # 588 cases today
scons sizecheck && scons portability && scons docscheck
./bin/benchmark.exe --check benchmark_baseline.txt
```

and if it touches generation, streaming or the mesh queue, the streaming probe
before and after — the numbers in ARCHITECTURE.md's sweep sections came from it,
and they are the difference between "this looks fine" and a `-30%` claim.

Two habits that make the answer trustworthy: **A/B both versions on one build**
(a temporary env toggle, shipped only long enough to measure, is what makes that
possible), and keep the losing measurement. The frustum pass's re-arm gate and
the backlog-scaled mesh budget were both "obviously wasteful" fixes that measured
*worse*, and both are recorded in [decisions/README.md](decisions/README.md).
