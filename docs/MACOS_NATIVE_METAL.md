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

1. The existing Vulkan host post-process produces a frame at the requested
   input size. One Vulkan GPU blit copies it to the shared placement-heap input.
2. Vulkan releases the input to external ownership and completes the submission
   before Metal reads it. The exact Metal device comes from the exported heap.
3. Optional MetalFX writes to a private output texture. A native Metal shader
   samples that output, or the shared input without MetalFX, directly into a
   `CAMetalLayer` drawable. The render pass clears letterbox bars to black and
   draws the settings overlay before presenting with display sync enabled.
4. Metal completes before Vulkan reacquires the shared input. The existing
   presentation-frame fence is signalled after the acquire, preserving safe
   frame reuse. Resize replaces a slot only after its previous reads finish.

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
the shared inputs alone use about 11 MiB. No measured speedup is implied.

The implementation follows [Apple's drawable/render-pass presentation flow](https://developer.apple.com/documentation/QuartzCore/CAMetalLayer)
and [Vulkan Metal external-memory interop](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_metal.html).
The driver still exposes no usable public shared-event bridge, so both APIs'
completion waits remain. A Vulkan swapchain object is retained for fallback and
existing format/frame setup; the native branch does not acquire or present it.

## Small runtime check

Run with `BB_FRAME_STATS=1`, enter offline gameplay, open and close the settings
menu once, then close the game normally. With MetalFX spatial enabled, check:

```bash
python3 - out/native-metal-check.log <<'PY'
from pathlib import Path
import re, sys
log = Path(sys.argv[1]).read_text(errors='replace')
assert 'first drawable completed successfully with MetalFX spatial' in log
assert 'Overlay: native Metal renderer ready' in log
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

## Next transition steps

Render the host frame directly into exportable storage to remove the remaining
input-copy blit, then validate a real captured game shader/workload on Metal.
The full backend still needs native resource/cache management, bindings,
graphics/compute pipelines and command submission. Geometry/tessellation and
guest completion semantics require their own proofs; presentation alone does
not establish full native rendering.
