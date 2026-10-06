# Experimental native Metal renderer transition

During graphics work, update [the game rendering and handling register](GAME_RENDERING_ISSUES.md)
with visual issues, direct game optimization candidates and their evidence.

Updated 2026-10-06. This is the first native Metal presentation stage of the
renderer transition. The game and host CPU code still run under Rosetta;
PS4 draw/compute commands and shader recompilation still use bbport's Vulkan
renderer through KosmicKrisp. Host SDR post-processing now has an opt-in native
Metal graphics pipeline; unsupported inputs retain the Vulkan pass.
Ordinary buffer storage/copies and selected color textures/copies/transfers now have
opt-in native Metal paths
described below; sparse guest arenas retain Vulkan ownership.

## Enable it

```bash
BB_PRESENT_BACKEND=metal BB_METALFX=off BB_RENDER_RES=1280x720 \
  BB_UPSCALER=off BB_FSR1=0 BB_RCAS=0 bash macos/run.sh
```

Use `BB_METALFX=spatial` in that command to upscale through MetalFX before the
native presentation pass. Equal-size and smaller windows use Metal sampling;
they do not invoke that scaler. `BB_METALFX_SCENE=1 BB_METAL_IMAGE_CACHE=1`
also enables the experimental scene upscale before the native-resolution HUD;
see the measured limitations below. The game HUD is otherwise part of the frame.
The settings menu and FPS counter use the existing ImGui Metal backend at
display resolution, with the same keyboard, mouse and controller input handling.

`BB_PRESENT_BACKEND=vulkan` selects the existing presentation path, which remains
the default. A missing interop extension or failed native allocation/command
selects Vulkan fallback and logs the reason. This initial native path is SDR;
HDR is unavailable while it is active. Native presentation enables display sync
and retains the unpatched game's 30 FPS timing. Menus can still run at 60 FPS.

## GPU path and lifetime

1. By default, Vulkan post-processing renders directly into the shared
   placement-heap frame. With `BB_METAL_POST_PROCESS=1`, supported RGBA8/BGRA8
   frames instead take an asynchronous, byte-preserving Vulkan image copy into
   that frame; Metal performs the color conversion on the presentation thread.
   The native presentation path preserves the display override's composed HUD
   resolution; ordinary scene frames retain the configured input size.
2. Vulkan releases the input to external ownership and completes the submission
   before Metal reads it. The exact Metal device comes from the exported heap.
3. The optional native host post-process writes to a private color texture.
   Optional MetalFX writes to a private output texture. A native Metal shader
   samples that output, the converted color or the shared input directly into a
   `CAMetalLayer` drawable. The render pass clears letterbox bars to black and
   draws the settings overlay before presenting with display sync enabled.
4. Metal completes before Vulkan reacquires the shared input. The existing
   presentation-frame fence is signalled after the acquire, preserving safe
   frame reuse. Window resize retains that shared input and replaces only the
   scaler/private output. Input-size changes replace the frame after its fence
   completes and its old Vulkan view is destroyed. On fallback, shared frames
   remain valid Vulkan sources until retirement.

The SDR frame is already sRGB encoded by the host post-process; the native
pass samples those values into a BGRA8 UNORM drawable. It does not read pixels
back to CPU memory. Drawable references stay within the per-frame autorelease
pool. Temporary drawable exhaustion skips the presentation attempt and retries
on a later frame.

Compared with the hybrid MetalFX path, native presentation omits the private-to-
shared output blit, shared Vulkan output allocation, and final Vulkan swapchain
blit. For a frame that actually has 720p input and 1080p output, three slots need about 34 MiB of interop
input/private-output pixel storage instead of 58 MiB, excluding alignment,
MetalFX internals, ordinary game frames and drawable storage. Without MetalFX,
the shared inputs alone use about 11 MiB. These inputs also serve as ordinary
post-processing frames, removing a separate frame allocation for every slot.
The full-resolution UI path uses larger inputs. No measured speedup is implied.

The implementation follows [Apple's drawable/render-pass presentation flow](https://developer.apple.com/documentation/QuartzCore/CAMetalLayer)
and [Vulkan Metal external-memory interop](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_external_memory_metal.html).
The driver still exposes no usable public shared-event bridge, so both APIs'
completion waits remain. A Vulkan swapchain object is retained for fallback and
existing format/frame setup; the native branch does not acquire or present it.

## Small runtime check

Run with `BB_FRAME_STATS=1`, enter offline gameplay, open and close the settings
menu once, then close the game normally. Pass `spatial` or `off` for the
presentation mode, or `scene` to require completed pre-HUD upscales:

```bash
python3 - out/native-metal-check.log spatial <<'PY'
from pathlib import Path
import re, sys
log = Path(sys.argv[1]).read_text(errors='replace')
mode = sys.argv[2]
assert mode in ('spatial', 'off', 'scene')
assert 'first drawable completed successfully' in log
if mode == 'spatial':
    assert 'first drawable completed successfully with MetalFX spatial.' in log
if mode == 'off':
    assert 'first drawable completed successfully.' in log
if mode == 'scene':
    assert len(re.findall(r'MetalFX scene #\d+: 1280x720 -> 1920x1080 before HUD', log)) >= 3
assert 'Overlay: native Metal renderer ready' in log
assert ('Vulkan frame input in shared' in log or
        'post-processing renders directly into shared' in log)
assert ('Vulkan render (no input copy)/completion' in log or
        'Vulkan snapshot/completion' in log)
assert not re.search(r'Native Metal bridge:.*?Vulkan copy/completion', log)
assert sum(int(n) >= 500 for n in re.findall(r'Guest flip stats: [\d.]+ FPS.*? (\d+) draws/frame', log)) >= 3
assert not re.search(r'Native Metal presentation unavailable|failed assertion|Assertion failed|SIGBUS|SIGSEGV', log)
print('Native presentation, overlay and gameplay progress recorded; ' +
      ('scene scaler completions checked' if mode == 'scene' else 'first drawable scaler mode checked'))
PY
```

The check requires at least three gameplay reporting windows; inspect the actual
image, controls and resize behavior separately. `spatial` proves its first
drawable, which can be a boot frame; use `scene` for pre-HUD gameplay scaler
completions. It is not a scanout or pacing
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

## Native host graphics pipeline: 2026-10-06

```bash
BB_PRESENT_BACKEND=metal BB_METAL_POST_PROCESS=1 BB_METALFX=spatial \
  BB_RENDER_RES=1280x720 BB_UPSCALER=off BB_FSR1=0 BB_RCAS=0 bash macos/run.sh
```

The opt-in Metal render pipeline implements the existing host SDR color
conversion, including sRGB views, RGBA/BGRA interpretation, gamma and opaque
alpha. PS4 geometry and game shaders still use Vulkan. The live replacement
accepts equal-size RGBA8/BGRA8 source/frame images with host FSR and HDR off;
other inputs use the original Vulkan pass. The standalone encoder also checks
packed 10-bit inputs, which the live snapshot shortcut currently excludes.

Vulkan copies the encoded image bytes asynchronously into the existing,
fence-protected shared frame. This prevents a later guest write from racing a
Metal read and lets the game command thread continue. The presentation thread
uses its existing external-ownership release/completion/acquire contract. Native
color conversion renders directly into the drawable when no presentation scaler
is needed, including scaling and letterboxing. The overlay shares that encoder
and render pass. This removes the intermediate color texture and second fullscreen
draw. When a larger window uses MetalFX, conversion still writes a tracked private
texture, followed by the scaler and presentation in the same command buffer. There is one Metal
submission/completion wait for those stages, and no CPU pixel copy. No public
shared-event bridge is available, so the presentation thread still waits for
Vulkan and Metal completion.

At 1920x1080 RGBA8, the scaler path adds a roughly 7.9 MiB private color texture per
slot (23.7 MiB for three slots); direct conversion into the drawable does not.
Both retain the 8.3 MB GPU snapshot per frame. This is a
native pipeline transition, not an established optimization. Failed native
conversion disables native presentation; queued unconverted frames retire
through their existing fences without being shown, and freshly prepared frames
use Vulkan post-processing. Runtime failure injection was not performed.
Fusing scaling with conversion also filters the source before gamma conversion,
whereas the previous two-pass path filtered the already-converted frame. Constant
color/letterbox checks and the native/Vulkan conversion fixtures pass; complete
pipeline pixel equivalence and moving-scene quality remain separate checks.

The prior native path reduced the composed 1080p HUD to the 720p scene size,
then enlarged it again. Frame allocation now preserves the display override's
1920x1080 size after its fence completes. Spatial MetalFX still scales the
complete composite for larger drawables; at equal or smaller sizes it is not
invoked. The optional scene scaler below runs before drawing the HUD.
The checked window initially prepared a 1710x961 target. With the preserved
1920x1080 UI input, gameplay uses native sampling rather than spatial MetalFX;
the successful presentation MetalFX message refers to the earlier 720p boot frame.
The scene option has its own completed-upscale counter. Retina drawable sizing
remains a separate audit.

The existing `metal-buffer-test` now compares native output with the original
Vulkan host pass after byte-preserving Vulkan snapshots. All 144 source-format,
view, output-format, gamma and explicit-decode combinations passed: 165,888
bytes compared, 192 one-LSB rounding differences, none larger. Asymmetric
rows/channels catch orientation and channel errors; alpha is checked separately.
Direct and threaded recording passed, as did the original Vulkan fallback
allocation/copy check. Logs: `out/macos-native-metal/post-process-{check,threaded-check,vulkan-check}.log`.

Before the direct-drawable change, the initial synchronous implementation recorded 18.9 FPS versus a 28.0 FPS
Vulkan reference in stationary Hunter's Dream, both with nominal pressure.
Moving conversion to the presentation thread recorded 26.1 FPS. The final
combined command was visually inspected in the title menu and Hunter's Dream,
survived window zoom/restore before measurement, and exited normally. Its longer
warmup run recorded 22.4 FPS, 50.00 ms median intervals and worst-window p99
100.00 ms. A subsequent Vulkan reference with the same 45-second warmup and
measurement recorded 28.3 FPS, 33.33 ms medians and worst-window p99 50.02 ms.
Both recorded nominal pressure and no compilation. The native snapshot/pass
path still regressed in this comparison; combining Metal submission did not
establish a speedup. These are sequential sessions, including resize before
the native measurement; nominal pressure does not establish equal GPU clocks
or rule out other load. Keep the native option off by default. Stable 30 FPS
and improved display pacing remain unproven.

Evidence under `out/benchmarks/`: `20261006-204515-506098-native-post-swizzle`,
`20261006-204806-173874-native-post-vulkan-reference`,
`20261006-205607-287469-native-post-present-thread`,
`20261006-210117-534512-native-post-combined`,
`20261006-210505-198243-combined-vulkan-reference`. Each completed with status 0;
no shader/pipeline compilation occurred during its measurement windows.

### Direct drawable and shared overlay pass

With no presentation scaler, host color conversion, aspect-fit scaling,
letterboxing and the settings overlay now share one native render encoder on the
drawable. The optional private post-process texture is released after its prior
command completes. Larger-window MetalFX retains the private conversion path.
There is still a Vulkan raw-frame snapshot to protect guest lifetime.

Stationary warm-cache sessions with original 30 FPS timing, 30-second warmup and
45-second measurement recorded:

| Host path | Guest FPS | Median / worst-window p99 | Measurement pressure | GPU command samples |
| --- | --- | --- | --- | --- |
| Vulkan host pass, native presentation reference | 27.6 | 33.33 / 50.02 ms | fair | 0.148-0.166 ms, presentation only |
| Native drawable conversion, separate overlay load pass | 25.0 | 33.33-33.34 / 50.02 ms | fair | 0.482-0.673 ms |
| Vulkan reference repeat | 27.5 | 33.33 / 50.02 ms | fair | retained in raw results |
| Native drawable conversion and overlay in one pass | 27.3 | 33.33 / 50.02 ms | fair | 0.452-0.468 ms |

These sequential sessions logged no compilation and comparable stationary draw
counts; they all became thermally **fair** after starting at nominal pressure.
The final native path has approached reference throughput in this observation,
but no isolated speedup, fixed clocks or stable 30 FPS are established. GPU
samples cover different command contents in Vulkan-host and native-host modes;
they are not whole-frame GPU time or per-shader comparisons. Keep native host
conversion off by default.

Evidence under `out/benchmarks/`: `20261006-213744-860914-direct-post-vulkan-reference`,
`20261006-214133-337492-direct-drawable-post`,
`20261006-214608-526894-direct-post-vulkan-repeat`, and
`20261006-215133-876161-direct-drawable-one-pass`. All four exited normally.
The two-pass drawable run visibly rendered the native settings overlay; the
final one-pass timing run completed through the same overlay backend with it hidden.
A separate correctness run, `20261006-215600-687520-direct-one-pass-ui-check`,
visibly rendered the settings overlay in the shared pass, survived window
zoom/restore during its 90-second warmup, and exited normally. Its 15-second
interval averaged 21.8 FPS with fair pressure and worst-window p99 66.67 ms.
The different warmup and UI interaction make it unsuitable for the comparison
above; the lower sustained throughput reinforces that stable 30 FPS is unproven.
Full image comparisons and moving-camera/gameplay validation remain open.

## Scene MetalFX before the HUD: 2026-10-06

```bash
BB_PRESENT_BACKEND=metal BB_METALFX=spatial BB_METALFX_SCENE=1 \
  BB_METAL_IMAGE_CACHE=1 BB_RENDER_RES=1280x720 BB_UPSCALER=off \
  BB_FSR1=0 BB_RCAS=0 bash macos/run.sh
```

`RunUiOnly` replaces the linear scene-background blit at the existing UI boundary.
Only a camera-identified finished RGBA8 scene qualifies; menus, unsupported
resources and failed scaler commands keep the Vulkan blit. Native conversion is
still disabled by default (`BB_METAL_POST_PROCESS=0`). The normal benchmark also
keeps scene MetalFX off; select it with `--env BB_METALFX_SCENE=1`.

The source and UI destination use the existing shared image cache. Reduced scene
proxies can now allocate through that cache and expose their borrowed Metal
handle from `SceneTargets::Read`. The ordinary source enters GENERAL through
`Runtime::Transit`, keeping the image layout tracker coherent even on fallback.
Vulkan releases both images and
`FinishForExternal` preserves staging reservations until completion. MetalFX
reads encoded tonemapped bytes through UNORM views in perceptual mode, writes a
cached private output, then GPU-blits into the shared UI attachment. Both images
return to Vulkan ownership even if scaling fails. Subsequent HUD/text draws stay
at 1920x1080. The driver still needs a synchronous command-thread completion
bridge here; pixels never pass through CPU memory.

The existing headless check covers four RGBA/BGRA UNORM/sRGB source formats,
changed input, two output sizes, constant-color quadrants for orientation/color/
alpha, rejected scaling, and finite GPU duration. It also checks direct host
conversion with black letterboxing and invalid viewport bounds. The earlier
144 native/Vulkan color-conversion comparisons still pass. Direct, deferred
recording and original Vulkan modes passed. Logs:
`out/macos-native-metal/scene-direct-{check,threaded-check,vulkan-check}.log`.

Hunter's Dream rendered with repeated `MetalFX scene ... 1280x720 -> 1920x1080
before HUD` completion messages, visible scene/HUD, and a clean exit in
`out/benchmarks/20261006-212923-357699-pre-hud-metalfx/`. The 60-second interval
averaged 18.2 FPS, median 50 ms and worst-window p99 150 ms, with no compilation
and 783-798 draws/frame. Thermal pressure became **fair**, so this run establishes
functionality and measured costs, not an isolated slowdown or speedup.
Repeated sparse samples logged 37.6-42.4 ms waiting for Vulkan release versus
0.46-0.60 ms of GPU upscale/copy work and 0.79-1.00 ms of Metal CPU completion
time. Keep it opt-in while addressing the command-thread bridge.

The final layout-tracking build repeated at least 600 scene completions and
exited normally in `20261006-220321-916073-pre-hud-layout-check`. This short
correctness run used 5-second warmup / 15-second measurement and no additional
cooldown. It recorded 20.8 FPS with nominal OS pressure; sparse samples showed
33.6-34.9 ms Vulkan release and 0.438-0.444 ms GPU upscale/copy. Nominal pressure
does not establish equal clocks, and these durations are not a controlled
performance comparison. All seven runs retained the 15 source-save file hashes.

Native presentation now logs completed command GPU durations and exact input,
target and drawable sizes. The benchmark retains those sparse samples in
`results.json`; they include all work in that command, not an isolated shader or
display scanout time. The first scene run predates that parser extension and
retains its samples in `game.log`. Timing is read after command completion using
[Apple's command-buffer GPU timestamps](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime).
The integration follows Apple's guidance to cache the scaler and upscale the
tonemapped scene in perceptual space
[before drawing the UI](https://developer.apple.com/videos/play/wwdc2022/10103/).

## Next transition steps

The host frame renders directly into exportable storage, and the real game
buffer-copy workload below passes on native Metal, including shared GPU buffers.
Ordinary cache buffers and selected color images now have the opt-in shared
ownership paths below. Guest
sparse arenas still require a different allocation/binding contract before
their shader bindings can move directly to Metal. Selected shared color-image
uploads/downloads have the opt-in Metal path below. Depth, compressed, volume
and multisample workloads still use Vulkan.
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
native Metal presentation and MetalFX off, using the existing uncapped
validation profile with its 60 FPS patch. The log recorded at least 900 native
buffer copies. A scene with roughly 1,360 draws/frame ran around 22 FPS, with
about 42 ms/frame spent completing Vulkan render work and 0.6 ms/frame in Metal
encode/completion. These are single-run CPU wall timings, not a matched baseline
or proof of a speedup. Stable 30 FPS remains unproven. The local run log is
`out/macos-native-metal/shared-cache-input-game.log`.

## Shared color-image cache and native copies (2026-10-06)

`BB_METAL_IMAGE_CACHE=1` gives eligible cache images one dedicated exported
placement heap with both Vulkan and Metal texture handles. The allocator is
also used by native presentation/MetalFX, so their memory/lifetime code is
shared. Vulkan views, rendering, uploads and downloads continue to use the same
image storage. Move construction/assignment transfers its sole owner and size;
retirement releases the Metal texture before destroying the Vulkan image and
freeing the allocation. Unsupported images retain the existing VMA path.

The initial scope is optimal, single-sample 2D color images, including mipmaps
and arrays: R8/RG8 UNORM, RGBA8/BGRA8 UNORM or sRGB, and R16/RG16/RGBA16 float.
The external format query and Metal/Vulkan size/alignment checks must pass.
The descriptor follows the driver's usage, mutable-format and compression
rules. Depth, compressed, 1D/3D, multisample and sparse images remain Vulkan.

`BB_METAL_IMAGE_COPY=1` uses a Metal blit for eligible cache image copies with
the same pixel format. Vulkan releases both images to external ownership in
GENERAL layout and completes a partial fenced submission. Metal completes the
copy before Vulkan reacquires their transfer layouts. The existing scheduler
tick is preserved, keeping unrelated staging allocations/callbacks live.
Invalid regions, overlapping destinations, format mismatches or Metal failure
use the original Vulkan copy. The command queue and completion helper are
shared with native buffer copies; neither path stages texture pixels on the CPU.
Both new settings default to off. Selected native texture uploads/downloads are
described below; batched Metal command submission remains unfinished.

```bash
BB_METAL_IMAGE_CACHE=1 BB_METAL_IMAGE_COPY=1 \
  BB_PRESENT_BACKEND=metal BB_METALFX=spatial bash macos/run.sh
```

The existing `metal-buffer-test` check now also covers all nine color formats,
move ownership, three mips/two array layers, partially covered destination
images and every untouched pixel. It preserves a pending staging tick and
callback across native copies, rejects invalid/overlapping copies, and checks
Vulkan format-copy fallback after external reacquire plus depth allocation
fallback. Direct, threaded and all-Vulkan modes passed on Apple M5; logs are
`out/macos-native-metal/shared-image-{check,threaded-check,fallback-check}.log`.

Live Central Yharnam rendered the character, scene and HUD with all four shared
cache/copy settings, native presentation and MetalFX spatial enabled. The run
selected `BB_FPS=30` and recorded at least 5,400 native image copies before
exiting with status 0. Its roughly 1,270-draw view ran around 16–18 FPS. This
proves live use of the path, not a performance gain or stable 30 FPS. Logs are
local under `out/macos-native-metal/shared-image-game.log`.

A short follow-up with shared images retained but `BB_METAL_IMAGE_COPY=0`
initially ran around 20–21 FPS from the same saved view. The camera later changed,
so this is not a controlled benchmark or proof of a speedup. It does suggest
that synchronous release/Metal-completion waits are too costly for this scene.
Native image copies remain off by default; batch submission and dependency
handling are the next performance work. The comparison log is
`out/macos-native-metal/shared-image-vulkan-copy-game.log`.

## Native color-image uploads and downloads (2026-10-06)

`BB_METAL_IMAGE_TRANSFER=1`, together with `BB_METAL_BUFFER_CACHE=1` and
`BB_METAL_IMAGE_CACHE=1`, sends eligible buffer/texture transfers through the
existing Metal blit queue. No extra bridge allocation or CPU texture upload is
introduced. Vulkan detiling and other compute work still precede these uploads.
The same nine uncompressed color formats, 2D mipmaps and array layers supported
by the shared image cache qualify. Buffer offsets, padded rows/slices and
subregions follow `VkBufferImageCopy`; invalid ranges, overlapping writes,
unsupported resources and Metal failure fall back to Vulkan. Row lengths above
16,384 pixels use Vulkan. Depth, compressed, volume and multisample images keep
their existing path.

The helper releases both shared resources, finishes the preceding Vulkan work
without advancing the scheduler tick, completes the Metal blit, then reacquires
Vulkan's transfer layouts. Staging reservations, callbacks and ordinary buffer
access tracking remain live until normal guest submission. This is still a
synchronous interoperability step, off by default, and makes no speedup claim.
Batching these transfers requires scheduler/dependency work next.

```bash
BB_METAL_BUFFER_CACHE=1 BB_METAL_IMAGE_CACHE=1 BB_METAL_IMAGE_TRANSFER=1 \
  BB_PRESENT_BACKEND=metal BB_METALFX=spatial bash macos/run.sh
```

The existing headless `metal-buffer-test` passed direct, deferred-recording and
Vulkan modes. It checks all nine formats, three mips/two layers, independent
Vulkan readback, padded rows/slices, subregion preservation, byte bounds,
overlapping-write rejection and reacquired Vulkan fallback. It also verifies
that native uploads preserve a pending staging reservation and callback.
Local logs: `out/macos-native-metal/image-transfer-{check,threaded-check,vulkan-check}.log`.

Two stationary 30-second gameplay runs used the same copied save, 720p scene,
spatial MetalFX and executable/GPU hashes, with a nominal-pressure cooldown
before each launch. Transfers off measured 25.1 FPS; transfers on measured
16.4 FPS and recorded at least 300 native uploads and 600 native downloads.
Both returned status 0, preserved the original save, showed stable draw counts
around 790/frame, and had no shader/pipeline compiles during measurement.
Both also reported **fair thermal pressure during measurement**, so the runner
flagged them. These single thermally flagged runs cannot establish an isolated
speedup or regression. The native path remains off by default because it is a
synchronous interoperability proof, with batching still unfinished.

Evidence: `out/benchmarks/20261006-151831-952274-thermal-transfer-off/` and
`out/benchmarks/20261006-152230-458970-thermal-transfer-on/`, including timing
windows, thermal samples and power-source metadata.

A separate short run, `out/benchmarks/20261006-152604-166928-metal-transfer-visual/`,
was used to inspect Hunter's Dream, the character, scene textures and HUD with
native transfers enabled. This confirms the observed rendering state, rather
than adding an FPS comparison.

## Native color-attachment clears (2026-10-06)

`BB_METAL_IMAGE_CLEAR=1` with `BB_METAL_IMAGE_CACHE=1` replaces eligible game
color clears with native Metal render passes over the existing shared texture.
Each selected mip/layer uses `MTLLoadActionClear` and `MTLStoreActionStore`;
there is no clone texture or CPU pixel transfer. This follows Apple's
[render-pass clear contract](https://developer.apple.com/documentation/metal/mtlrenderpasscolorattachmentdescriptor/clearcolor).
It covers the cache's nine uncompressed color formats and single-sample 2D
textures/arrays. Empty or out-of-bounds ranges, non-color aspects, non-finite
clear values, unsupported storage and native command failure retain Vulkan
fallback. Depth, compressed and multisample storage still uses Vulkan.

The runtime releases only the selected subresources to GENERAL, completes
preceding Vulkan work without retiring the guest tick, runs Metal, then records
the Vulkan acquisition. Staging reservations and callbacks remain live until
ordinary submission. The clear and blit paths reuse one native command queue
and completion helper. This remains synchronous and off by default; native
draw pipelines and batching are unfinished.

```bash
python3 tools/benchmark_macos.py --seconds 30 --label native-color-clear --env BB_METAL_IMAGE_CLEAR=1
```

The existing `metal-buffer-test` passed direct, threaded and Vulkan modes. It
compares every byte against independent Vulkan clear/readback commands across
all nine formats, including sRGB and half-float, three mips and two layers. It
checks partial-range preservation, rejected ranges/non-finite values and pending
staging/callback lifetime. Logs:
`out/macos-native-metal/image-clear-{check,threaded-check,vulkan-check}.log`.

The automatic gameplay check
`out/benchmarks/20261006-155126-270220-native-color-clear/` reached Hunter's Dream,
visually retained the character/scene/HUD, logged at least 6,600 native clears,
measured 20 seconds after five seconds of workload warmup and exited normally
with status 0. Original save hashes were unchanged. The run selected PS4 30 FPS
timing but averaged 16.05 guest FPS across its two complete measurement windows.
Thermal pressure rose to fair and the runner flagged the result. This single
run does not isolate the clear-path cost or establish a speedup; the path stays
off by default.
