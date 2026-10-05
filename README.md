# Metalborne

**Bloodborne meets Metal.** An experimental Apple Silicon macOS fork of
[bbport](https://github.com/deadinside28/bloodborne_pc).

Metalborne keeps bbport's game-specific runtime and GPU translation, adapting
its x86-64 executable to macOS and running the game through Rosetta 2. Its
renderer uses Vulkan through KosmicKrisp; native MetalFX spatial upscaling is an
opt-in experiment. A complete native Metal renderer and ARM64 game translation
are research directions, not implemented features.

**Target: the original game's 30 FPS with better frame pacing.** This is a
source development project, not a stable release. The game has reached title
screens and gameplay on an Apple M5, but crashes, rendering errors and frame
rates below 30 remain. Results from the upstream Linux port do not establish
macOS performance or compatibility.

## Build and run on macOS

You need an Apple Silicon Mac with Rosetta 2, Apple's command-line developer
tools, CMake, Ninja, Python 3, an x86-64 Vulkan loader and a compatible Vulkan
Metal driver. Follow the [macOS setup and limitations](docs/MACOS.md) for the
supported development environment and driver configuration.

```bash
git clone --recurse-submodules https://github.com/Livio21/metalborne.git
cd metalborne
bash scripts/build_macos.sh
BB_GAME_DIR=/absolute/path/to/CUSA03173 bash macos/run.sh
```

Supply your own **decrypted Bloodborne CUSA03173 v1.09** dump, including the
required game modules. No game files, saves, downloaded runtime binaries or
shader caches are distributed here. Build outputs and local game data belong
under the ignored `out/` directory.

## Port features and current work

- Darwin memory, guest TLS, synchronization, threading, audio and Cocoa window
  integration while retaining bbport's runtime.
- [Keyboard and mouse controls](docs/CONTROLS.md), including mouse camera input.
- Conservative GPU settings for the macOS driver and an original 30 FPS default.
- [Guest and host presentation timing](docs/MACOS_PERFORMANCE.md) to distinguish
  game flips, queue delays and display-side submission costs.
- [Experimental MetalFX spatial upscaling](docs/MACOS_METALFX.md), disabled by
  default, with Vulkan fallback when interop requirements fail. No temporal
  MetalFX or frame generation is implemented.
- [GPU optimization and native Metal research](docs/MACOS_GPU_RESEARCH.md).

Report macOS fork issues in [Metalborne's issue tracker](https://github.com/Livio21/metalborne/issues).
Include your Mac, macOS version, Vulkan driver version, launch options and a
log with personal paths removed.

## Credits and license

This is a fork of **deadinside28's bbport**, which supplies the runtime,
offline linking and game-specific renderer optimizations. Its GPU renderer and
shader recompiler are derived from [shadPS4](https://github.com/shadps4-emu/shadPS4).
The existing dependency credits and licenses remain in the source tree.

See the [original upstream README](docs/UPSTREAM_README.md) for Linux features,
instructions, detailed acknowledgements and upstream development information.
The project retains its [GNU GPL v2 license](LICENSE). Metalborne is not
affiliated with Sony Interactive Entertainment, FromSoftware, Apple, AMD,
shadPS4 or the upstream bbport project.
