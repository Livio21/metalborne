# macOS performance investigation — 2026-10-04

Repeatable automatic gameplay measurements are available through
`python3 tools/benchmark_macos.py --seconds 30`; see
[the benchmark runbook](MACOS_BENCHMARK.md) for settings, results and caveats.

## Reused bbport-mac optimizations (2026-10-06)

Metalborne now incorporates the small KosmicKrisp list-restart optimization,
FSR unified-memory fallback and parallel pipeline warmup from
[bmy/bbport-mac at 0a25a43](https://github.com/bmy/bbport-mac/tree/0a25a43693fb03b645eada115f16365148dafc4a).

- KosmicKrisp list restart is disabled by default, avoiding the driver's indexed
  list unrolling path. Strip restart still follows the existing topology rules.
  Set `BB_LIST_RESTART=1` before launch to restore extension negotiation for a
  comparison. Other drivers keep their existing extension selection.
- FSR's Vulkan backend can use a host-visible/device-local memory type if no
  invisible device-local type matches. Discrete-memory preference, memory-type
  masks and the AMD device-coherent-memory feature guard are retained. The patch
  is applied by both build scripts; existing FSR profiling edits are preserved.
- Cached pipeline creation uses up to four workers on macOS. Store reads,
  shared shader metadata and map insertion remain on the warmup thread. Raw
  pipeline-key bytes are retained, including padding. `BB_PRELOAD_THREADS=1`
  restores serial loading; explicit values 1 through 16 are accepted. Invalid
  values fall back to the default. Cached unsupported geometry/tessellation
  stages are rejected before driver creation.

The x86-64 macOS build succeeded. `python3 tests/test_macos_gpu_reuse.py` checks
the production memory selector, worker-count parser and completion loop with
stubbed device memory and jobs. This checks policy and ordering, not actual GPU
shader correctness. Live startup subsequently loaded 475 cached pipelines on
four workers in 9.5 seconds (9.3 seconds in the parallel creation phase), then
prepared native Metal presentation and entered gameplay with spatial MetalFX.
There is no matched serial startup measurement yet.

After the user reported gameplay loaded, four complete windows at a similar
workload (1,323-1,350 draws/frame) reported 21.9-24.1 guest Flip FPS, averaging
23.3. Guest interval p99 ranged from 50.01 to 83.33 ms; host presentation-call
interval p99 ranged from 47.02 to 81.79 ms. There were no shader/pipeline compiles.
Later windows had changing draw counts, so they are unsuitable for a scene
comparison. These are guest/API measurements, not display scanout; neither a
speedup nor stable 30 FPS is established. The captured windows are retained in
`out/macos-native-metal/bbport-reuse-stationary.json`.

The driver disk-cache patch has not been applied: this workspace uses a
prebuilt KosmicKrisp binary, while the patch changes Mesa source. Temporal
MetalFX and experimental parallel draw recording were not imported in this
stage. Native graphics pipelines and the full renderer transition remain open.

Local evidence: `out/macos-bbport-reuse-build.log` and
`out/macos-native-metal/bbport-reuse-game.log`.

## Setup

Apple M5, 16 GiB unified memory, 10 CPU cores (4 performance / 6 efficiency),
macOS 27.0.1. The x86-64 RelWithDebInfo/LTO build runs under Rosetta with
KosmicKrisp from the official shadPS4 v0.19.0 macOS release. Game: the user's
CUSA03173 v1.09 extraction and existing save. Upscaling off, unpatched 30 FPS
game timing, default 1920 × 1080 game resolution. Measurements use
`BB_FRAME_STATS=1`; no GPU timestamp profiling was enabled.

## Observations

| Run / stage | Result | Interpretation |
| --- | --- | --- |
| Default optimized renderer, menus | Approximately 60 FPS | Menu/video workload; does not establish gameplay FPS. |
| Default optimized renderer, loading | One 3432.9 ms frame; 8 shader/pipeline compiles totaling 3381.1 ms in that reporting window | Compilation is a substantial loading hitch. |
| Default optimized renderer, map loading | Exited 23 on unimplemented PM4 type 0 | No valid steady gameplay baseline from this run. |
| `BB_COPY_GPU_BUFFERS=1`, map loading | Exited 23 on malformed packet length / PM4 type 1 | Copying top-level command buffers did not resolve the assertion. |
| `BB_PREP_WORKERS=0`, DrawPipeline disabled, initial stationary room view | Repeated 5-second windows at 19.2–19.3 FPS; median 50 ms, p99 about 66.7 ms | Real gameplay rendering, below the 30 FPS target. These windows reported zero shader/pipeline compiles. |
| Same conservative run, later windows | About 15 FPS, then 21–25 FPS as the draw workload changed | Scene and system conditions varied; do not attribute these differences solely to a toggle. |
| DrawPipeline enabled live, preparation workers still disabled | Around 15–16 FPS initially; no assertion during this short loaded-scene interval | No clear speedup established. This does not verify optimized map loading. |

The conservative launch reached the world and continued to the six-minute
watchdog limit without the earlier PM4 assertion. The watchdog's thread-dump
phase then logged a guest SIGBUS and exited 138; this was not a clean shutdown,
so do not use it as evidence that shutdown works correctly. The initial loading
failures and this single successful boot justify a temporary workaround;
they do not prove the exact cause or general stability. DrawPipeline was
enabled briefly in the middle of the run and then disabled again.

## Where time is going

Follow-up source inspection on 2026-10-05 established that these frame intervals
are measured in guest `Flip` processing, normally before a separate swap thread
finishes presentation. They measure guest flip throughput, not confirmed screen
cadence. The GPU-tick wait also includes intentional `BB_FRAMES_AHEAD` backpressure.
See [GPU handling and direct Metal research](MACOS_GPU_RESEARCH.md) for the
30 FPS pacing target, measurement plan and cached-shader conversion evidence.

In representative initial room windows, the GPU command thread reported:

- About 63–64% of the reporting window waiting for GPU timeline ticks.
- About 21% waiting for host copies, roughly 296–299 waits per frame.
- About 11.6–11.8 microseconds CPU time per draw, around 1450 draws per frame.
- Almost no time waiting for the copy-worker pool itself.

These are CPU-side elapsed wait counters, **not GPU utilization percentages**.
They implicate GPU submission/completion and the ordering of host copies.
They do not distinguish expensive GPU passes from driver overhead or a
submission dependency that keeps the GPU waiting.

A three-second CPU sample captured `Scheduler::WaitHostCopies` yielding,
`Scheduler::Wait` waiting through KosmicKrisp / Metal shared events, and Vulkan
recording work in Metal render-encoder creation and draw calls. It also caught
pipeline creation through Metal's compiler, so the short stack sample should
not be presented as a compilation-free sample. The separate steady frame
windows above reported no measured compilations.

The process physical footprint was about 7.7 GiB (7.9 GiB peak); RSS alone
was only about 1.25 GiB and understates that footprint. Texture-cache reports
placed tracked allocations around 3.4–3.5 GiB, above their pressure threshold,
with no images evicted. System-wide compressed memory and swap were present.
Over one roughly 58-second interval, swap-ins increased by 2039 pages
(about 32 MiB), swap-outs did not increase, and compression/decompression
counters advanced substantially. These system-wide counters cannot assign
the activity to this game or prove paging caused the FPS drop.

## Temporary launch default

`bash macos/run.sh` now selects `BB_PREP_WORKERS=0` and disables
`BbToggle::DrawPipeline` via a launcher-owned toggle file. Other renderer
optimizations remain available. This is a containment workaround and does
not claim a repaired PM4 race or a measured FPS gain.

To restore the prior optimized defaults for diagnostics:

```bash
BB_MACOS_CONSERVATIVE_GPU=0 BB_FRAME_STATS=1 bash macos/run.sh
```

Explicit `BB_PREP_WORKERS` and `BB_TOGGLE_FILE` values override the corresponding
conservative defaults. Ordinary launcher runs have no diagnostic timeout.

## Next work, in order

1. Isolate the map-loading PM4 failure with preparation workers enabled and
   DrawPipeline disabled, then the inverse, across repeated boots. Audit
   speculative guest-memory reads and fence ordering before restoring defaults.
2. Compare an identical stationary scene at 1080p and 720p, then profile GPU
   passes separately. Timestamp profiling can itself change render-pass
   behavior, so record its overhead and keep those results separate.
3. Reduce expensive submission / host-copy synchronization while preserving
   the order of writes and fences visible to the game. Waiting counters alone
   are insufficient justification for removing a wait.
4. Investigate memory pressure and live resource retention on 16 GiB machines.
5. Improve pipeline warmup and its progress display. `PipelineCache::WarmUp`
   already recreates pipelines from cached keys and shader data at startup;
   unseen variants still compile during play. Investigate driver cache
   persistence where supported. This targets first-use hitches; steady FPS
   requires the other work above.

## Local evidence

- `out/macos-performance-baseline.log`
- `out/macos-performance-copybuffers.log`
- `out/macos-performance-conservative.log`
- `out/macos-performance-conservative-sample.txt`

Logs and the driver/game/cache files are local ignored artifacts. No comparison
against shadPS4 gameplay performance has been measured.

## Presentation diagnostics and 30 FPS defaults (2026-10-05)

The macOS launcher now requests `BB_PRESENT_MODE=Fifo` by default, alongside
`BB_FPS=30`, the unpatched game timing. An explicit environment setting overrides
it. The frames-ahead bound remains 1. FIFO is a vsync presentation policy; this
change alone does not establish an FPS gain or evenly spaced screen updates.

`BB_FRAME_STATS=1` now labels the existing report `Guest flip stats`. The same
flag also enables host presentation diagnostics, unless `BB_PRESENT_STATS=0` is
specified. `BB_PRESENT_STATS=1` can enable host timing without the detailed guest
stall logs.

New reports distinguish:

- Guest flip intervals from host presentation-call completion intervals.
- Time waiting in the swap-thread work queue from time spent executing its work.
- Acquisition/resize, optional MetalFX, record/submit and queue-present API stages.
- The intentional frames-ahead wait from the aggregate GPU timeline wait counter.

Host completion intervals include invocation overhead and skipped presentation
attempts; they are not a display scanout measurement. `record/submit` covers API
calls that may defer driver work to the recorder, rather than pure driver CPU
time. All instrumentation is opt-in.

See [MetalFX integration](MACOS_METALFX.md) for the experimental spatial pass and
its current CPU synchronization cost. It is disabled by default, and no new
matched-scene performance result is claimed for these changes.

## MetalFX gameplay check (2026-10-05)

The experimental spatial path reached Hunter's Dream gameplay on the M5 with
1280x720 input and 1710x961 output. Two stationary windows reported 29.1-29.2
guest Flip FPS, median 33.33 ms and p99 50.01 ms. Host presentation-call p50 was
33.75-33.82 ms, p99 39.27-39.51 ms; these are API timings, not screen scanout.
There were no shader/pipeline compiles in those windows.

The Vulkan copy/completion wait averaged 24.85-25.63 ms/frame and the Metal
encode/completion stage 1.18-1.23 ms/frame. The former includes waiting for
rendered input, so it is not the pure cost of the copy. The run closed through
the window with exit status 0. This scene differs from the earlier starting-room
baseline; a speedup and stable 30 FPS have not been established. MetalFX remains
off by default. See [runtime evidence and allocation findings](MACOS_METALFX.md).

Local evidence: `out/macos-metalfx-run/spatial-private-output.log`.

## Recording-mode comparison (2026-10-05)

With the character and camera stationary in the heavier starting-room scene
(about 1380 draws/frame), four baseline windows averaged 14.53 guest Flip FPS;
five windows with threaded recording disabled averaged 14.86 FPS. This small
difference does not establish a gain, so threaded recording remains enabled.
Earlier windows with a moving camera, and windows collected during compilation,
are excluded. Copy-pool routing also has no valid matched-scene gain.

Local evidence: `out/macos-optimization/before.log` and `phases.jsonl`.

### Blocking host-copy waits on macOS

`Scheduler::WaitHostCopies` now uses C++ atomic wait on macOS. The recorder wakes
the waiter after each command batch, retaining the existing release/acquire
completion sequence and guest fence ordering. Other platforms retain yielding.
Notifying after every small copy was rejected after a slower gameplay run;
notifications are batched instead. Waiting can extend to the end of the batch.

The build succeeded, and the batch-wait version reached the same stationary
starting-room view with about 1380 draws/frame, 1280x720 rendering, MetalFX off,
FIFO presentation and the conservative GPU settings. The last six warm windows
of the previous-renderer run averaged 17.57 guest Flip FPS and 14.06 microseconds
of GPU command-thread CPU time per draw; six warm batch-wait windows averaged
19.57 FPS and 7.76 microseconds/draw. This is about 45% less command-thread CPU
time per draw in this short comparison. The guest pacing median remained 50 ms;
the 30 FPS target is not met in this scene. These are separate runs, not a
sustained performance guarantee or a display scanout measurement. No compiler
was running during either comparison window.

Local evidence: `out/macos-optimization/baseline-repeat.log`, `batch-wait.log`
and `batch-wait-build.log`. A smoke check for forward progress through host-copy
waits, after entering gameplay and leaving it still for at least 30 seconds:

```bash
python3 - out/macos-optimization/batch-wait.log <<'PY'
import pathlib, re, sys
log = pathlib.Path(sys.argv[1]).read_text(errors='replace')
rows = re.findall(r'Guest flip stats: ([\d.]+) FPS.*?([\d.]+) draws/frame.*?host copies ([\d.]+)% \(([\d.]+)/frame\)', log)
game = [tuple(map(float, row)) for row in rows if float(row[1]) >= 500]
assert len(game) >= 3, 'Need three gameplay windows; check for a stalled waiter'
assert all(fps > 0 and waits > 0 for fps, draws, blocked, waits in game[-3:])
assert not re.search(r'Assertion failed|SIGBUS|SIGSEGV', log)
print('Gameplay progressed through host-copy waits; check visuals separately')
PY
```

### User-supplied patch collection

The supplied `Bloodborne.xml` was imported locally as
`out/macos-run/patches/Bloodborne-user.xml`, with all its entries disabled in
`out/macos-run/patches.json`. It has not replaced the bundled XML or been
published. The existing external-patch loader can select individual entries.

The 192 literal writes in `Performance Patch (Perf Increase)` fit the v1.09
ELF loadable segments and do not conflict with the bundled 1280x720 writes.
These checks do not establish runtime compatibility or an FPS gain. The
`rgba8f color space` patch changes game render-target precision; `lower specific
renders` lowers selected pass resolutions. Both require a visual comparison.
`30 FPS++` changes game timing, and the faster-loading patch requests vblank
500 or higher. Neither is enabled for the original 30 FPS / 60 Hz target.
