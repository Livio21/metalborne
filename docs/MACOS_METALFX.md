# Experimental MetalFX spatial integration

Updated 2026-10-05. The target remains the original game's **30 FPS**, with
consistent delivery at roughly **33.33 ms per game frame**.

## Current status

The macOS renderer now contains an opt-in MetalFX spatial pass between its
post-processed game frame and its final Vulkan swapchain blit. This retains
bbport's PS4 command handling and shader recompiler. It is a hybrid Vulkan/Metal
path, not a complete native Metal backend or an ARM64 game executable.

The x86-64 macOS build links Apple's MetalFX framework and uses
`MTLFXSpatialScaler`. GPU pixels remain on the GPU, using shared interop textures and a private MetalFX output. It does not read
frames back to CPU memory. **The title screen and Hunter's Dream gameplay have rendered with the path active.
A performance improvement and long-run stability are not established.** The normal launcher leaves it off.

The installed KosmicKrisp driver on this Apple M5 reports:

- `VK_EXT_external_memory_metal` available.
- `VK_EXT_metal_objects` unavailable.
- RGBA16F images with sampled, storage, color attachment and transfer usage:
  Metal **texture** handle query fails with `VK_ERROR_FORMAT_NOT_SUPPORTED`.
- The same image query with a Metal **heap** handle succeeds, reporting
  exportable and importable memory (`external_features=0x6`).

Evidence: `out/macos-metal-research/interop-capabilities.txt`. An extension name
alone does not establish support for every handle type in that extension.

## Runtime evidence (2026-10-05)

On the Apple M5 with KosmicKrisp 0.19.0, the corrected shared-heap/private-output
path prepared **1280x720 -> 1710x961 RGBA8** and logged the first completed GPU
upscale. The title image, menu and credits displayed without obvious corruption.
A subsequent offline load reached the Hunter's Dream, with the character and
HUD visible. Two stationary windows reported **29.1-29.2 guest Flip FPS**, guest
median 33.33 ms and p99 50.01 ms; host presentation-call p50 was 33.75-33.82 ms
and p99 39.27-39.51 ms. There were zero shader/pipeline compiles in those windows.
This is a different scene from the earlier room baseline and establishes no
matched-scene speedup or actual display scanout cadence. The process closed
through its window with exit status 0.

Representative title windows reported roughly 2-6 ms per frame for the entire
MetalFX stage after its initial warmup. These are CPU wall times including waits;
they are neither a gameplay speedup nor pure GPU scaler timings.

Evidence: `out/macos-metalfx-run/spatial-private-output.log`, an ignored local log.

## Allocation findings (2026-10-05)

The capability query did not guarantee usable MetalFX storage. A real allocation
exported a placement heap with **shared** storage. Requiring a private exported
heap therefore caused fallback. Creating a private native heap and importing it
triggered a driver assertion: its resource creation still requested shared
storage. That attempted import path was removed.

The implementation keeps the driver's shared placement heaps and allocates a
separate private MetalFX output on the exact Metal device obtained from the
exported heap. A Metal blit returns its output to the Vulkan-shared image.

## How the spatial pass works

1. Allocate dedicated Vulkan RGBA8 images with exportable Metal heap backing.
2. Export each heap and create a matching Metal placement texture at offset zero.
   Check placement heap type, texture size, alignment and usage. Shared storage
   is accepted for the interop textures.
3. Blit the post-processed game image into the shared input image on the GPU.
4. Release the input/output images to external ownership and wait for Vulkan
   completion before Metal accesses them.
5. Encode MetalFX into its required private output texture, then GPU-blit that
   into the shared output texture and wait for Metal completion.
6. Acquire the images back into Vulkan and blit the result into the swapchain.
   Vulkan draws the emulator overlay at display resolution afterward.

Dedicated resources are kept per presentation frame and replaced only after
that frame's previous Vulkan reads have completed. MetalFX's fence is supplied
for untracked heap resources. Resource lifetime extends through completion of
both APIs. Idle redraws reuse the last completed upscale rather than rerunning
MetalFX or switching to a differently filtered source. Unsupported allocation/scaler requirements produce a diagnostic
and fall back to the normal Vulkan image.

The current presentation frame is SDR data that the post-processing shader has
already sRGB encoded into an UNORM target. The RGBA8 textures preserve
those values and MetalFX uses **perceptual** color processing. HDR bypasses this
initial path. Letterboxing follows the game's image aspect ratio. The game HUD
is part of the input image and is upscaled with it; only the emulator overlay
is drawn afterward at display resolution.

## Enable it

From this workspace:

```bash
BB_METALFX=spatial BB_RENDER_RES=1280x720 BB_METALFX_INPUT_RES=1280x720 \
  BB_UPSCALER=off BB_FSR1=0 BB_RCAS=0 BB_FRAME_STATS=1 \
  bash macos/run.sh
```

`BB_RENDER_RES` uses the existing startup resolution patch and actually changes
what the game renders. `BB_METALFX_INPUT_RES` controls the host image supplied to
MetalFX; setting only that variable does **not** lower all of the game's rendering
work. It defaults to `BB_RENDER_RES` when supplied, otherwise 1920x1080.

The scaler runs only when the swapchain's fitted image is larger than the input.
For example, use a 1280x720 input and a 1920x1080 window. Equal sizes and
smaller windows use the regular Vulkan path. Resizing adjusts the destination
size without changing the game's startup resolution patch.

Keep bbport's existing temporal upscaler off for this initial integration.
`BB_METALFX=off` disables the pass. Other MetalFX modes print an unsupported-mode
message and use normal presentation. No MetalFX mode is selected automatically.

Look for these distinct messages:

- `MetalFX spatial requested`: the feature was requested at startup.
- `MetalFX spatial prepared`: textures and the scaler were created.
- `MetalFX spatial: first GPU upscale completed successfully`: the first Metal
  command buffer completed. This still does not prove visual correctness.
- `MetalFX spatial unavailable`: the fallback was selected, with a reason.

A small runtime check, after launching with logging and closing normally:

```bash
BB_METALFX=spatial BB_RENDER_RES=1280x720 BB_FRAME_STATS=1 \
  bash macos/run.sh > out/metalfx-check.log 2>&1
python3 - <<'PY_CHECK'
from pathlib import Path
log = Path("out/metalfx-check.log").read_text(errors="replace")
assert "MetalFX spatial: first GPU upscale completed successfully" in log
assert "MetalFX spatial unavailable" not in log
assert "failed assertion" not in log
PY_CHECK
```

This checks activation and command completion; inspect the actual image as well.

## Synchronization cost and remaining work

This version has a **CPU completion bridge** between Vulkan and Metal. That
adds submission/wait boundaries, an input conversion blit and an output blit. It can erase the
benefit of rendering at a lower resolution; a speedup must be measured in the
same scene. Scratch textures also consume extra memory. At 720p input and 1080p output,
three slots of RGBA8 input, shared output and private output use approximately
58 MiB of pixel storage, plus alignment and MetalFX's internal resources.
`BB_METALFX_FORMAT=rgba16f` selects 16-bit float scratch textures for comparison
and doubles this pixel storage; `rgba8` is the default.

`BB_PRESENT_STATS=1` reports host queue delay, completion intervals, API-stage
wall times and the MetalFX bridge's copy/completion and encode/completion times.
`BB_FRAME_STATS=1` includes these reports by default. They are CPU wall times,
not GPU utilization, pure GPU pass duration or confirmed display scanout timing.

Next steps are broader visual/runtime validation, matched-scene measurements and a GPU
synchronization bridge to replace these CPU waits. The installed driver exposes
Vulkan external semaphore/fence FD support, but no public Metal shared-event
export API has been established. Removing waits without another ordering
mechanism would permit one API to read images while the other is writing them.

MetalFX temporal upscaling needs a correctly matched color/depth/motion-vector
stream, jitter, disocclusion handling and history resets. It is not implemented
by this spatial pass. Frame interpolation is also not implemented; the current
priority is 30 real game frames per second with consistent delivery.

## API references

- [Apple MetalFX spatial scaler](https://developer.apple.com/documentation/metalfx/mtlfxspatialscaler)
- [Khronos external Metal memory](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_metal.html)
- [Metal placement texture API](https://developer.apple.com/documentation/metal/mtlheap/maketexture(descriptor:offset:))
- [Broader GPU and native Metal research](MACOS_GPU_RESEARCH.md)
