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

The v1.09 **Skip Intro + warning message** patch is enabled by default and
recorded in the run metadata. Override it with `--env BB_PATCHES=` to disable
it. The default menu delay remains 12 seconds. Navigation first clamps to Play
Online with Up, then selects Play Offline with Down; the saved menu selection
can otherwise send a run into the login-failure dialog. The runner loads the
copied save through Continue.

Defaults match the current experimental setup: PS4 30 FPS game timing, FIFO,
1280x720 scene, native Metal presentation, presentation spatial MetalFX, shared buffer/image
caches, native buffer copies, Vulkan image copies/transfers/clears/host
post-processing and conservative
draw preparation. The renderer still uses Vulkan for game draws. Actual output size
is recorded in `game.log`.

Before launch, the runner waits for 30 seconds of nominal macOS thermal pressure
(`--cooldown 0` skips that wait). It samples Foundation's `NSProcessInfo.thermalState`
and Low Power Mode every five seconds through startup, warmup and measurement.
Results retain the samples and flag comparisons if any run sample is above
nominal, unknown, or Low Power Mode is on/unavailable. Power-source/settings
information is also retained. Cooldown uses the startup-timeout value as its own
separate deadline; a timeout launches no game.

This is OS thermal pressure, not a temperature or CPU/GPU frequency trace.
Nominal samples do not prove clocks were constant or exclude competing work.
Keep the same power source, display size, power mode and background workload;
alternate settings across repeated runs before attributing small FPS changes.
Elevated thermal pressure is evidence of a confounder, not proof of its exact
contribution. Earlier benchmark reports have no thermal samples and cannot be
retrospectively cleared of this confounder. Apple's
[thermal-state guidance](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power_efficiency_guidelines_osx/RespondToThermalStateChanges.html)
describes the API and its limits.

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
python3 tools/benchmark_macos.py --seconds 60 --label metal-transfers --env BB_METAL_IMAGE_TRANSFER=1
python3 tools/benchmark_macos.py --seconds 60 --label metal-host-pass --env BB_METAL_POST_PROCESS=1
python3 tools/benchmark_macos.py --seconds 60 --label metal-scene --env BB_METALFX_SCENE=1
python3 tools/benchmark_macos.py --manual --seconds 30
python3 tools/benchmark_macos.py --self-check
```

For a recorded walking/camera route, pass `--replay /absolute/path/route.txt`.
The runtime's existing `BB_PAD_RECORD` / F9 mechanism creates recordings with
eight values per line: milliseconds, buttons, four stick axes and two triggers.
The recording must cover the measurement duration. It starts after warmup;
without a route, all stick axes stay centered for a stationary benchmark.

For the native host pass, inspect `Native Metal post-process` or
`Native Metal direct post-process` messages in
`game.log`; an enabled flag alone does not prove supported frames took that
path. The input should retain the composed UI size (1920x1080); direct conversion
scales it once to the drawable's aspect-fit target. The larger-window scaler
path retains `1920x1080 -> 1920x1080` during host conversion.
`Vulkan snapshot/completion` includes the asynchronous
GPU copy of encoded bytes into the shared frame. It is not a CPU pixel readback.
Resize and overlay checks belong before measurement or in a separate run.
For scene MetalFX, require completed `MetalFX scene ... before HUD` messages;
a boot presentation message alone does not establish use in gameplay.

## Results

Each `out/benchmarks/<timestamp>-<label>/` contains:

- `game.log`: complete raw output, including startup compilation and timing.
- `metadata.json`: chip, OS, Git revision, dirty state, executable/GPU hashes,
  save hashes, settings and benchmark durations.
- `results.json`: completion/failure status, exit status, all measured guest
  and host timing windows, sparse completed native GPU samples and the summary.
- `windows.csv`: guest FPS, draw count, compilation time, median, standard
  deviation, p99 and slow-frame count for each accepted reporting window.
- `user/`, `bbport.ini`, `pad.txt`: the isolated save/settings and input file.

The first partially overlapping five-second reporting window is excluded.
FPS is the mean of reported window FPS values. `worst_window_p99_ms` is the
largest window p99, not a pooled percentile across all frames. Guest flips
measure game throughput; host timings measure presentation API calls. Neither
is a display scanout measurement. `workload_changed` flags draw-count spread
above 10%; that threshold cannot establish that two runs show the same scene.
Native GPU samples are first completions and every 300th command, filtered to
the measurement interval. Scene samples include the upscale and private-output
copy; presentation samples include the selected post/scaler/overlay passes.
Their release/completion wall times include CPU waits. Missing GPU timestamps
remain `null`; do not treat these sparse samples as per-frame percentiles.

Startup timeout, an early exit, lack of timing progress, insufficient workload
windows or forced shutdown produce a failed result and nonzero runner exit.
Raw logs and any available measurements are retained. Benchmark duration does
not include preparation, pipeline warmup, menu navigation or workload warmup.

## Verified automatic run — 2026-10-06

The original run below predates thermal monitoring. Its FPS is a record of that
session; heat-related throttling cannot be ruled out retrospectively. Two later
30-second runs with monitoring enabled recorded fair pressure during measurement
and were correctly flagged for comparison. Both kept Low Power Mode off and
AC power attached. See the native Metal runbook for their results.

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

The boot-skip verification in
`out/benchmarks/20261006-154247-348453-boot-skip-verified/` automatically reached
Hunter's Dream with the four community patch writes active, measured 10 seconds,
and closed normally with status 0. Original save hashes were unchanged. Thermal
pressure rose to fair during the run, so its FPS is not evidence of a speedup.
