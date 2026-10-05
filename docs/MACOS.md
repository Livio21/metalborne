# Experimental Apple Silicon port

This branch starts from bbport commit
`5224a6d137d8c4efaab69f2412f4bb86227b9ab8`.
The intended execution path is:

```text
PS4 x86-64 game and native PS4 libraries
  → bbport x86-64 loader and game-specific runtime, under Rosetta
  → bbport's optimized GPU command pipeline and shader recompiler
  → Vulkan driver for macOS
  → Metal / Apple GPU
```

The executable is x86-64, including SDL, FFmpeg and the renderer dependencies.
An arm64 executable cannot directly enter the game's x86-64 code. The Vulkan
loader and driver must also contain an x86-64 slice.

## Build

Requirements: Apple Silicon, Rosetta, macOS 26 or newer, current Xcode Command
Line Tools, Git, Python 3, CMake, Ninja, and an x86-64 Vulkan loader and driver.
The deployment target defaults to macOS 26. Dependencies are fetched at pinned
revisions into `out/`; arm64 Homebrew graphics libraries are not linked.

```bash
git submodule update --init --recursive
export BB_VULKAN_LOADER=/absolute/path/to/libvulkan.1.dylib
export VK_DRIVER_FILES=/absolute/path/to/driver_icd.json
bash build.sh
```

On macOS `build.sh` selects `scripts/build_macos.sh`. The Vulkan loader can
also be supplied as CMake's `Vulkan_LIBRARY` argument, and Vulkan headers are
fetched automatically. Set `BB_BUILD_JOBS` to control build concurrency.
The script uses CMake and Ninja from PATH, or an existing local installation
in `out/macos-tools/bin`.

For this workspace, a universal loader and MoltenVK driver were already
installed with vkcube. Those paths are machine-specific:

```bash
export BB_VULKAN_LOADER=/Applications/vkcube.app/Contents/Frameworks/libvulkan.1.4.357.dylib
export VK_DRIVER_FILES=/Applications/vkcube.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
```

Both MoltenVK and KosmicKrisp passed Vulkan command submission and buffer
readback on this Apple M5. The real game title screen was rendered using the
x86-64 KosmicKrisp driver bundled with official shadPS4 v0.19.0. MoltenVK has
not been checked with a full game boot.

## Run with your own game dump

The port expects your decrypted **CUSA03173 v1.09** folder. No game content is
included. With the same Vulkan environment set:

```bash
BB_GAME_DIR=/absolute/path/to/CUSA03173 bash run.sh
```

For the macOS defaults, launch with your own game directory:

```bash
BB_GAME_DIR=/absolute/path/to/CUSA03173 bash macos/run.sh
```

The macOS launcher currently disables speculative draw-preparation workers and
the parallel draw pipeline by default after PM4 assertions during map loading.
This is a temporary workaround, not a confirmed repair of the underlying race.
Set `BB_MACOS_CONSERVATIVE_GPU=0` to restore the renderer defaults for diagnostics.
Explicit `BB_PREP_WORKERS` and `BB_TOGGLE_FILE` settings take precedence.
See [the measured performance report](MACOS_PERFORMANCE.md).

Saves, settings, prepared binaries and shader caches are kept in
`out/macos-run`. See [keyboard and mouse controls](CONTROLS.md) for bindings,
mouse capture and sensitivity. The scratch development `out/Bloodborne.app`
needs runtime arguments and is not a distributable double-click launcher.

Preparation runs on the host and keeps guest TLS loads in FS on macOS. Always
regenerate prepared images on macOS; Linux's generated images use GS instead.
The GTK Linux launcher and AppImage packaging are not macOS launchers.

## CPU diagnostic build

```bash
bash scripts/build_macos.sh --cpu-only
```

This produces `out/bb-probe-cpu` without a GPU backend. It is for loader and
runtime diagnostics; it cannot render or play the game. Use its `--cpu-only`
option when supplying a prepared image. This separate target avoids mistaking
a CPU-only link success for a working graphics port.

## Platform adaptations

- Guest FS uses an LDT mapping modeled on shadPS4's macOS TLS implementation;
  Darwin retains its own GS state.
- Runtime allocations use reusable low-address slabs so guest-visible handles
  fit PS4's 40-bit pointer fields. Guest direct/flexible allocations start at
  `0x7000000000`, beyond Rosetta's reserved lower region.
- POSIX shared memory replaces Linux memfd backing. Guest aliases share the
  same storage. Released storage is zeroed on macOS.
- Monotonic condition waits, absolute sleeps, timed locks, thread names,
  signal contexts and per-thread CPU statistics use Darwin equivalents.
- Cocoa window creation and event polling stay on the main thread. Guest
  initialization and execution move to a worker after graphics initialization.
- Vulkan portability enumeration, Metal surfaces and portability-subset
  device extension selection are enabled for macOS. Explicit driver environment
  settings are preserved.
- bbport's two-stage draw processing, cache optimizations and sparse register
  scanning remain in the renderer. Sparse scans use an explicit word array on
  libc++ because GNU's bitset scan methods are unavailable.

## Evidence and remaining work

The complete macOS build produces `out/bb-probe` and
`out/macos/gpu/libbbgpu.dylib`, both x86-64 Mach-O binaries, including bbport's
real Vulkan renderer and shader recompiler. Build success, Vulkan device
initialization, command submission/readback, native PS4 libc and Fios2
initialization, and rendering the real Bloodborne title menu have been
confirmed on Apple M5 under Rosetta with KosmicKrisp.

The x86-64 CPU diagnostic builds on Apple Silicon and executes under Rosetta.
The 14 existing synthetic loader cases passed. Platform checks exercised low
allocations, FS thread-local state across 16 workers, and occupied mapping
protection. Runtime checks passed shared direct-memory aliasing, release
clearing, callback lifecycle, ABI guards, synchronization and resolver scope.
The later large-allocation reuse change has been compiled but has not been
exercised by those checks.
POSIX shared-memory checks require execution outside Codex's filesystem sandbox.

An observed startup freeze was sampled at `sceAvPlayerStop`: the guest main
thread was waiting to join a decoder while the demuxer also joined it. The
host thread shim now serializes external joins and leaves self-joins to the
owner. A subsequent run passed that frozen background and map loading and
rendered the hunter, starting room and HUD. Additional shader compilation
caused stalls during the first load. Movement and combat, camera feel,
audible sound, save round trips, controller
behaviour still require verification. The starting room has now been measured
at approximately 19 FPS in the conservative configuration, with later windows
dropping to around 15 FPS. Broader gameplay performance remains unverified.
No macOS FPS comparison
against shadPS4 is claimed. Timed-lock polling, zeroing released backing storage
and the low-address allocator need profiling with a real game workload.

Apple's [Game Porting Toolkit](https://developer.apple.com/games/game-porting-toolkit/)
can provide porting examples and Metal diagnostics. Its Metal shader converter
takes DXIL; this renderer emits SPIR-V. Using that converter would require a
new graphics backend. The current branch uses the Vulkan-to-Metal route.

## MetalFX and presentation timing

`BB_PRESENT_BACKEND=metal` selects experimental native Metal presentation, with
optional MetalFX and the native settings overlay. PS4 rendering still uses
Vulkan, and the default presentation backend remains Vulkan. See the
[native Metal launch options and remaining backend work](MACOS_NATIVE_METAL.md).
That document also covers an opt-in real-game Metal compute comparison, shared
GPU buffer interop, and a guarded buffer-copy shortcut validated against the
original shader's output. The full PS4 renderer transition remains unfinished.

The macOS launcher targets the unpatched 30 FPS game with FIFO presentation.
Experimental MetalFX spatial upscaling is available through `BB_METALFX=spatial`;
it has rendered the title and Hunter's Dream, and remains disabled by default
while broader stability and matched-scene performance validation are pending. See [MetalFX integration and launch options](MACOS_METALFX.md) and
[presentation diagnostics](MACOS_PERFORMANCE.md#presentation-diagnostics-and-30-fps-defaults-2026-10-05).
