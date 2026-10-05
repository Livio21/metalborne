# macOS performance investigation — 2026-10-04

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
