# Experimental native Metal presentation

Updated 2026-10-06. This is the first native Metal presentation stage of the
renderer transition. The game and host CPU code still run under Rosetta;
PS4 draw/compute commands, shader recompilation, resource caches and host
post-processing still use bbport's Vulkan renderer through KosmicKrisp.

## Enable it

```bash
BB_PRESENT_BACKEND=metal BB_METALFX=off BB_RENDER_RES=1280x720 \
  BB_UPSCALER=off BB_FSR1=0 BB_RCAS=0 bash macos/run.sh
```

Use `BB_METALFX=spatial` in that command to upscale through MetalFX before the
native presentation pass. Equal-size and smaller windows use Metal sampling;
they do not invoke the spatial scaler. The game HUD is part of the game frame.
The settings menu and FPS counter use the existing ImGui Metal backend at
display resolution, with the same keyboard, mouse and controller input handling.

`BB_PRESENT_BACKEND=vulkan` selects the existing presentation path, which remains
the default. A missing interop extension or failed native allocation/command
selects Vulkan fallback and logs the reason. This initial native path is SDR;
HDR is unavailable while it is active. Native presentation enables display sync
and retains the unpatched game's 30 FPS timing. Menus can still run at 60 FPS.

## GPU path and lifetime

1. The existing Vulkan host post-process renders at the requested input size
   directly into the shared placement-heap frame. Its attachment/pipeline format
   matches the exported texture. There is no intermediate frame-to-input blit.
2. Vulkan releases the input to external ownership and completes the submission
   before Metal reads it. The exact Metal device comes from the exported heap.
3. Optional MetalFX writes to a private output texture. A native Metal shader
   samples that output, or the shared input without MetalFX, directly into a
   `CAMetalLayer` drawable. The render pass clears letterbox bars to black and
   draws the settings overlay before presenting with display sync enabled.
4. Metal completes before Vulkan reacquires the shared input. The existing
   presentation-frame fence is signalled after the acquire, preserving safe
   frame reuse. Window resize retains that shared input and replaces only the
   scaler/private output. Input-size changes replace the frame after its fence
   completes and its old Vulkan view is destroyed. On fallback, shared frames
   remain valid Vulkan sources until retirement.

The SDR frame is already sRGB encoded by the Vulkan post-process; the native
pass samples those values into a BGRA8 UNORM drawable. It does not read pixels
back to CPU memory. Drawable references stay within the per-frame autorelease
pool. Temporary drawable exhaustion skips the presentation attempt and retries
on a later frame.

Compared with the hybrid MetalFX path, native presentation omits the private-to-
shared output blit, shared Vulkan output allocation, and final Vulkan swapchain
blit. At 720p input and 1080p output, three slots need about 34 MiB of interop
input/private-output pixel storage instead of 58 MiB, excluding alignment,
MetalFX internals, ordinary game frames and drawable storage. Without MetalFX,
the shared inputs alone use about 11 MiB. These inputs also serve as ordinary
post-processing frames, removing a separate frame allocation for every slot.
No measured speedup is implied.

The implementation follows [Apple's drawable/render-pass presentation flow](https://developer.apple.com/documentation/QuartzCore/CAMetalLayer)
and [Vulkan Metal external-memory interop](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_metal.html).
The driver still exposes no usable public shared-event bridge, so both APIs'
completion waits remain. A Vulkan swapchain object is retained for fallback and
existing format/frame setup; the native branch does not acquire or present it.

## Small runtime check

Run with `BB_FRAME_STATS=1`, enter offline gameplay, open and close the settings
menu once, then close the game normally. Pass `spatial` or `off` to match the
MetalFX mode used for that run:

```bash
python3 - out/native-metal-check.log spatial <<'PY'
from pathlib import Path
import re, sys
log = Path(sys.argv[1]).read_text(errors='replace')
mode = sys.argv[2]
assert mode in ('spatial', 'off')
assert ('first drawable completed successfully' +
        (' with MetalFX spatial.' if mode == 'spatial' else '.')) in log
assert 'Overlay: native Metal renderer ready' in log
assert 'post-processing renders directly into shared' in log
assert 'Vulkan render (no input copy)/completion' in log
assert not re.search(r'Native Metal bridge:.*?Vulkan copy/completion', log)
assert len(re.findall(r'Guest flip stats: [\d.]+ FPS.*? [1-9]\d{2,} draws/frame', log)) >= 3
assert not re.search(r'Native Metal presentation unavailable|failed assertion|Assertion failed|SIGBUS|SIGSEGV', log)
print('Native presentation, MetalFX, overlay and gameplay progress recorded')
PY
```

The check requires at least three gameplay reporting windows; inspect the actual
image, controls and resize behavior separately. It is not a scanout or pacing
measurement. `Native Metal bridge` reports CPU wall time including render
completion, drawable acquisition and Metal encoding/completion waits.

## Runtime evidence: 2026-10-06

The x86-64 macOS build completed on Apple M5 / 16 GiB. Native presentation
without MetalFX reached gameplay and exited with status 0. A second run with
MetalFX spatial rendered Central Yharnam, including the character and HUD, and
the settings overlay rendered through ImGui's Metal backend. The macOS Help key
generates Insert for opening/closing the overlay. A window zoom resize continued
to display the scene correctly. The second run also exited with status 0.

The runtime check above passed against the local ignored log
`out/macos-native-metal/spatial.log`: 37 gameplay reporting windows ranged from
19.6 to 28.2 FPS. The character/camera and scene changed during this session;
these values establish forward progress, not a comparison with Vulkan or a
speedup. Later Central Yharnam windows reported roughly 41–42 ms waiting for the
Vulkan copy/render completion and 1.2–1.4 ms for Metal encoding/completion. These
are CPU wall times, not isolated GPU execution times. Stable 30 FPS and improved
scanout pacing remain unproven.

### Direct shared-frame follow-up

The input-copy removal also built successfully. The updated check passed on
`out/macos-native-metal/direct-frame-spatial.log`, confirming direct shared-frame
allocation and `Vulkan render (no input copy)/completion` reports. Central
Yharnam, the character/HUD, and the native settings overlay were visually
inspected. Gameplay continued after window zoom resize; the run exited with
status 0. Runtime injection of a native allocation/command failure was not
performed.

Six later gameplay windows measured 21.7–24.7 FPS, with about 35.8–37.5 ms in
the Vulkan completion bridge and 1.25–1.28 ms in Metal encoding/completion for
the last three reports inspected. Scene/camera changes and overlay/resize
interaction prevent a matched performance comparison. The remaining Vulkan
completion wait includes the game's rendering workload; removing the input
blit does not remove that wait or establish stable 30 FPS.

A final build with the ownership-acquire access scope covering both subsequent
attachment writes and Vulkan fallback reads was run with MetalFX off. The same
runtime check passed on `out/macos-native-metal/direct-frame-off.log`; Central
Yharnam and HUD were visually inspected, the overlay was opened/closed, and the
run exited with status 0. Five later reporting windows measured 25.6–26.5 FPS
at roughly 1263–1270 draws/frame, versus roughly 1340 draws/frame in the spatial
run above. These are different views/workloads and cannot establish the cost of
MetalFX or a speedup from removing the blit.

## Next transition steps

The host frame now renders directly into exportable storage. Next validate a
real captured game shader/workload on Metal.
The full backend still needs native resource/cache management, bindings,
graphics/compute pipelines and command submission. Geometry/tessellation and
guest completion semantics require their own proofs; presentation alone does
not establish full native rendering.
