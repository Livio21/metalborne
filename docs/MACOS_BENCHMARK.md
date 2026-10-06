# Scripted macOS benchmark

From the repository root, after building with `bash build.sh`:

```bash
python3 tools/benchmark_macos.py --seconds 30
```

The script launches through `macos/run.sh`, selects Play Offline, acknowledges
the offline dialog and selects Continue,
waits for three consecutive reporting windows above 500 draws/frame, warms up
for 15 seconds, records the requested interval, then requests the normal window
close path. This workload gate is a heuristic; it does not read the game's menu
or player state. A new save, a different title-menu selection or unexpected
dialogue may need `--manual` or a longer `--menu-delay`.

Defaults match the current experimental setup: PS4 30 FPS game timing, FIFO,
1280x720 scene, native Metal presentation, spatial MetalFX, shared buffer/image
caches, native buffer copies, Vulkan image copies and conservative draw
preparation. The renderer still uses Vulkan for game draws. Actual output size
is recorded in `game.log`.

Each run copies the existing save directory into its own output folder. Game
save writes and settings changes go to that copy. The existing shader cache is
reused by default, making this a warm-cache benchmark. `BB_GPU_USER_DIR` can be
overridden to compare a separate cache. The original save is fingerprinted in
the metadata. Automatic runs ignore normal keyboard/gamepad input; close the
window to interrupt, or interrupt the runner with Ctrl+C. The runner refuses to
launch alongside another `bb-probe` process.

## Comparing settings

Run these separately, with the same original save and no other changes:

```bash
python3 tools/benchmark_macos.py --seconds 60 --label restart-off
python3 tools/benchmark_macos.py --seconds 60 --label restart-on --env BB_LIST_RESTART=1
```

Other examples:

```bash
python3 tools/benchmark_macos.py --seconds 60 --label serial-preload --env BB_PRELOAD_THREADS=1
python3 tools/benchmark_macos.py --seconds 60 --label vulkan --env BB_PRESENT_BACKEND=vulkan --env BB_METALFX=off
python3 tools/benchmark_macos.py --manual --seconds 30
python3 tools/benchmark_macos.py --self-check
```

For a recorded walking/camera route, pass `--replay /absolute/path/route.txt`.
The runtime's existing `BB_PAD_RECORD` / F9 mechanism creates recordings with
eight values per line: milliseconds, buttons, four stick axes and two triggers.
The recording must cover the measurement duration. It starts after warmup;
without a route, all stick axes stay centered for a stationary benchmark.

## Results

Each `out/benchmarks/<timestamp>-<label>/` contains:

- `game.log`: complete raw output, including startup compilation and timing.
- `metadata.json`: chip, OS, Git revision, dirty state, executable/GPU hashes,
  save hashes, settings and benchmark durations.
- `results.json`: completion/failure status, exit status, all measured guest
  and host timing windows and the summary.
- `windows.csv`: guest FPS, draw count, compilation time, median, standard
  deviation, p99 and slow-frame count for each accepted reporting window.
- `user/`, `bbport.ini`, `pad.txt`: the isolated save/settings and input file.

The first partially overlapping five-second reporting window is excluded.
FPS is the mean of reported window FPS values. `worst_window_p99_ms` is the
largest window p99, not a pooled percentile across all frames. Guest flips
measure game throughput; host timings measure presentation API calls. Neither
is a display scanout measurement. `workload_changed` flags draw-count spread
above 10%; that threshold cannot establish that two runs show the same scene.

Startup timeout, an early exit, lack of timing progress, insufficient workload
windows or forced shutdown produce a failed result and nonzero runner exit.
Raw logs and any available measurements are retained. Benchmark duration does
not include preparation, pipeline warmup, menu navigation or workload warmup.

## Verified automatic run — 2026-10-06

On this Apple M5 / 16 GiB machine, the script automatically reached the saved
Hunter's Dream scene, passed the workload gate, warmed up for 15 seconds,
measured 30 seconds and closed through the window event loop with exit status 0.
Five complete measurement windows averaged 19.3 guest FPS (18.2–20.7), with
50.00 ms median intervals and a worst window p99 of 216.67 ms. Host-call worst
window p99 was 142.51 ms. Draw counts stayed at 787–790 per frame and there were
no shader/pipeline compiles during measurement. This remains below the 30 FPS
target; these values do not establish a speedup or display frame pacing.

Evidence: `out/benchmarks/20261006-150411-922614-automated-metal-spatial/`.
The run used the development app's freshly copied `bb-probe`; its executable
is identical to the newly built `out/bb-probe`, which is the script's default.
The earlier calibration correctly rejected the title menu and closed cleanly
on startup timeout. The parser self-check and macOS build also passed.
