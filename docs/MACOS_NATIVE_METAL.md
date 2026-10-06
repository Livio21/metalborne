# Experimental native Metal renderer transition

Updated 2026-10-06. This is the first native Metal presentation stage of the
renderer transition. The game and host CPU code still run under Rosetta;
PS4 draw/compute commands, shader recompilation, textures and host
post-processing still use bbport's Vulkan renderer through KosmicKrisp.
Ordinary buffer-cache storage and copies now have an opt-in native Metal path
described below; sparse guest arenas retain Vulkan ownership.

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

The host frame renders directly into exportable storage, and the real game
buffer-copy workload below passes on native Metal, including shared GPU buffers.
Ordinary cache buffers now have the opt-in shared ownership path below. Guest
sparse arenas still require a different allocation/binding contract before
their shader bindings can move directly to Metal, followed by texture workloads.
The full backend still needs native resource/cache management, bindings,
graphics/compute pipelines and command submission. Geometry/tessellation and
guest completion semantics require their own proofs; presentation alone does
not establish full native rendering.

## Native game compute proof

An opt-in shadow dispatch validates Bloodborne's two-buffer copy shader
`0x3d5ebf4e` against its actual Vulkan output. It requires an exact byte match
between the supplied SPIR-V and the module compiled for the live pipeline,
including cached modules and overrides. A shader hash alone is insufficient.
Only direct 64x1x1 dispatches with two valid, non-overlapping buffer ranges and
bounded accesses qualify. Image, sampler, DMA and indirect workloads are excluded.

The existing scheduler completes a GPU readback before the real Vulkan dispatch
and another afterward. Native Metal compiles the locally translated MSL through
`newLibraryWithSource`, binds cloned buffers through a tier-2 argument buffer,
uses the real push constants/group count, and compares every buffer byte with
the Vulkan reference. Both written and untouched bytes are checked. The game's
live resources continue to use the original Vulkan result, even on mismatch.
The diagnostic runs once per process, is off by default, and retains no captured
resources on disk. Readbacks and runtime compilation can cause a visible pause.

Generate the MSL from your own locally cached module with an installed
SPIRV-Cross CLI, then run:

```bash
spirv-cross "$SPV" --msl --msl-version 30100 --msl-argument-buffers \
  --msl-argument-buffer-tier 2 --output out/compute-copy.metal
BB_METAL_COMPUTE_SPV="$SPV" BB_METAL_COMPUTE_MSL="$PWD/out/compute-copy.metal" \
  BB_PRESENT_BACKEND=metal BB_METALFX=off BB_FRAME_STATS=1 bash macos/run.sh
```

`$SPV` must be the copy module from your own cache. Game-derived shaders and
buffer contents must remain local. This uses native runtime compilation and
requires no Xcode Metal command-line compiler.

One runnable check, after entering gameplay and closing normally:

```bash
python3 - out/compute-proof.log <<'PY'
from pathlib import Path
import re, sys
log = Path(sys.argv[1]).read_text(errors='replace')
assert 'exact SPIR-V module matched; shader 000000003d5ebf4e' in log
m = re.search(r'Native Metal compute proof: PASS; 2 buffers, (\d+) bytes compared, (\d+) bytes changed, 0 mismatches;', log)
assert m and int(m[1]) > 0 and int(m[2]) > 0, 'Need a successful dispatch with a nonzero effect'
assert len(re.findall(r'Guest flip stats: [\d.]+ FPS.*? [1-9]\d{3,} draws/frame', log)) >= 3
assert not re.search(r'compute proof: FAILED|compute proof: MISMATCH|failed assertion|Assertion failed|SIGBUS|SIGSEGV', log)
print('Real Metal compute output matched Vulkan and gameplay continued')
PY
```

On Apple M5, `out/macos-native-metal/compute-proof.log` recorded 7,864,320
compared bytes across two buffers, 226,410 changed bytes, zero mismatches,
15,360 groups and 64 threads/group. Metal reported 0.079 ms for this dispatch.
Central Yharnam, the character and HUD were visually inspected afterward; the
run exited with status 0. This proves one real game compute workload, not a
replacement for Vulkan compute or the full renderer. The diagnostic's copies,
waits and compilation are not included in that GPU time; no gameplay speedup
or stable 30 FPS is established.

An intentionally incorrect local MSL variant writing zeros recorded 226,410
mismatches in `out/macos-native-metal/compute-proof-negative.log`. The game
continued to render Central Yharnam and exited with status 0. The runnable check
rejects that log. This exercises the output comparison without modifying the
live Vulkan resources.

The installed KosmicKrisp 0.19 driver also reported external-memory features
`0x6` (import/export) for ordinary Metal-heap buffers, but `0x0` for sparse
Metal-heap buffers with storage/transfer/device-address usage. bbport's guest
buffer cache uses sparse arenas. A direct native compute replacement therefore
needs a different buffer ownership contract or a GPU copy bridge; ordinary
buffer export alone cannot share the existing sparse arenas. The local probe
is `out/macos-native-metal/buffer-capabilities.c`; the specification's
[Metal external-memory API](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_metal.html)
defines the resource export mechanism, while the installed driver query
establishes this limitation.

### GPU buffer bridge and optional copy shortcut

`BB_METAL_COMPUTE_BRIDGE=gpu` populates the native proof buffers with Vulkan GPU
copies into exported coherent placement heaps. Metal uses native buffers over
those same allocations; no CPU upload populates them. Both APIs complete and
transfer ownership explicitly, with a Metal fence for untracked heap resources.
The diagnostic still reads back the original inputs and Vulkan reference for
byte comparison. Failed shared-buffer allocation falls back to snapshot clones.
It does not replace the game's live compute dispatch.

`BB_BUFFER_COPY_HLE=1` additionally checks a GPU buffer-copy alternative against
the original shader. It poisons the cloned destination before the copy and
compares all bytes, including untouched ranges. Only after both proofs pass can
later matching dispatches use the existing `Runtime::CopyBuffer` helper. The
shortcut checks the exact shader module, resource roles, workgroup shape, bound
ranges, alignment and non-overlap. It preserves the cache's GPU-write
invalidation and transfer-access barrier tracking. A failed proof keeps the
original compute path. This is an opt-in Vulkan buffer-copy optimization; it
uses the driver's transfer path rather than a direct native Metal dispatch.
It adds no diagnostic readbacks or API completion waits to subsequent copies.

Use the existing proof command with `BB_METAL_COMPUTE_BRIDGE=gpu`, and optionally
`BB_BUFFER_COPY_HLE=1`. For the optional shortcut, add these assertions to the
same runnable check after the successful compute comparison:

```python
assert 'GPU copies into shared buffers; no CPU uploads to Metal' in log
assert 'Buffer copy HLE proof: PASS; poisoned destination then copied' in log
assert 'verified shader replaced by GPU buffer copy #1' in log
assert 'Buffer copy HLE proof: MISMATCH' not in log
```

The first shared-buffer run, `out/macos-native-metal/compute-bridge.log`, matched
the same 7,864,320 bytes with zero mismatches. Central Yharnam and HUD were
visually inspected and the run exited with status 0. Metal returned invalid GPU
timestamp data for this run; its timing is excluded from performance evidence.
The diagnostic now reports GPU timing as unavailable when the timestamp pair
is missing, reversed or outside a plausible duration. Scene changes prevent a
matched FPS comparison.

The final build's `out/macos-native-metal/compute-copy-hle.log` passed both the
shared-buffer Metal comparison and the poisoned-destination GPU-copy comparison.
At least 7,500 later live dispatches used the guarded shortcut, including
3,932,160- and 5,013,504-byte copies. Central Yharnam, the character and HUD were
visually inspected, the runnable check passed, and the run exited with status 0.
This establishes correctness and removal of compute binding/dispatch commands
for those copies. Scene/camera changes prevent a measured speedup claim.

The final negative run, `out/macos-native-metal/compute-copy-hle-negative.log`,
used the shared GPU bridge, the intentionally incorrect local MSL, and the
shortcut flag. The Metal comparison recorded 226,410 mismatches; no shortcut
was enabled. Central Yharnam and HUD continued to render through the original
compute path, and the run exited with status 0. A log assertion confirmed both
continued gameplay and the absence of any live shortcut calls.

### Shared ordinary buffer cache and native copies

`BB_METAL_BUFFER_CACHE=1` makes ordinary upload, readback, stream and device
buffers use a single coherent allocation shared by Vulkan and Metal. It reuses
the existing `Buffer`, `StreamBuffer` and staging-pool lifetimes. Vulkan device
addresses refer to those allocations; host mappings refer to the same storage.
Allocation, export or layout incompatibility falls back to the original VMA
allocator. Shared allocations are limited to 512 MiB each. Sparse arenas retain
their original allocation and aliasing behavior.

`BB_METAL_BUFFER_COPY=1` additionally sends eligible ordinary-buffer copies
through native Metal blit commands over that cache storage. These live copies
need no clone allocation, bridge upload or CPU pixel readback. Same-buffer,
unaligned, out-of-bounds and overlapping destination regions are rejected by
the native helper; unsupported copies and Metal failures use the original
Vulkan path. Normal Vulkan caller validity requirements still apply.

Vulkan releases ownership and completes a partial submission under a fence.
This waits for earlier GPU work and deferred host uploads without signaling the
scheduler's logical tick. Stream reservations, staging entries, descriptors and
deferred callbacks therefore remain protected until the eventual ordinary
submission. Metal completes synchronously before Vulkan acquisition is recorded.
Completed callbacks run outside the queue submit lock and can submit a nested
native page-table upload without deadlocking. This is deliberately synchronous;
it establishes ownership correctness and does not establish a performance win.
Both settings are off by default.

```bash
BB_METAL_BUFFER_CACHE=1 BB_METAL_BUFFER_COPY=1 \
  BB_PRESENT_BACKEND=metal BB_METALFX=off bash macos/run.sh
```

One headless check uses no game-derived data and exercises the production paths:

```bash
out/macos-tools/bin/cmake --build out/macos --target metal-buffer-test --parallel 4
export VK_DRIVER_FILES="$PWD/out/vendor/kosmickrisp-0.19.0/kosmickrisp_mesa_icd.json"
out/macos/gpu/metal-buffer-test
out/macos/gpu/metal-buffer-test --threaded
out/macos/gpu/metal-buffer-test --vulkan
```

On Apple M5, the direct and threaded checks passed shared mappings/device
addresses, move ownership, a 128 MiB stream allocation, offset copies and every
untouched byte. They also checked that a native partial submission preserves
unrelated staging reservations and callbacks, that a completed callback can
submit another native copy, and that a rejected byte-aligned copy falls back
before its staging ring is overwritten. The third invocation passed the
original allocation/copy path. Logs are local under
`out/macos-native-metal/shared-cache-{check,threaded-check,fallback-check}.log`.

The rebuilt game also reached offline gameplay with both settings enabled,
native Metal presentation and MetalFX off. The log recorded more than 900 native
buffer copies. A scene with roughly 1,360 draws/frame ran around 22 FPS, with
about 42 ms/frame spent completing Vulkan render work and 0.6 ms/frame in Metal
encode/completion. These are single-run CPU wall timings, not a matched baseline
or proof of a speedup. Stable 30 FPS remains unproven. The local run log is
`out/macos-native-metal/shared-cache-input-game.log`.
