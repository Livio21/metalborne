# Game rendering and handling register

Updated 2026-10-09. Keep this register alongside the native Metal transition.
For each new visual defect, rendering cost or game-behavior issue, record its
scene/reproduction, build and settings, evidence, suspected owner, proposed
change, visual/gameplay cost and validation result. Update existing entries
when evidence changes. A source finding, another game's technique or an upstream
report is a candidate until reproduced here. Preserve the 30 FPS simulation
target and original gameplay behavior when evaluating optimizations.

## Current priorities

| ID / priority | Finding and evidence | Owner / status | Next action and acceptance |
| --- | --- | --- | --- |
| GAME-01 / high | `BB_FPS=30` applies no FPS patch; the XML separately contains `30 FPS++`. A stationary Hunter's Dream comparison recorded 23.9 FPS with that patch versus 24.6 FPS without it; both runs had fair thermal pressure. illusion documents a game-limiter/platform-flip interaction in Bloodborne and Dark Souls III [1]. | Game timing; no isolated benefit established. Original timing remains the default. | Repeat with fixed display/scene/thermal conditions. Measure actual presentation and input latency; check animation, cloth, particles and physics during late frames. API-call timing is not scanout. Avoid multiple competing limiters. |
| GAME-02 / high | The single `1080p Light Grid` patch with 720p scene / 1080p guest UI reduced reported draws from roughly 790 to 680 in Hunter's Dream (about 14%). The first comparison had fair pressure. A later nominal-pressure run retained 679-688 draws/frame but averaged 26.7 FPS, below the preceding unpatched reference's 28.3 FPS. The broad Performance Patch was absent. | Game lighting workload; draw reduction reproduced, speedup not demonstrated. Lighting correctness remains unchecked. Opt-in. | Keep guest grid/output, UI, viewport and Retina drawable sizes distinct. Check light boundaries, shadows and camera movement, then repeat alternating runs under comparable power/thermal conditions. Fewer draws alone do not establish less GPU time or better frame pacing. |
| GAME-03 / medium | Motion blur, dynamic shadows, DoF, SSAO, AA and model LOD patches already exist. Their XML notes describe visual compromises; the motion-blur patch also removes velocity-map rendering. | Game effects; candidates, not correctness fixes. | Measure one pass/option at a time. Prefer lower pass resolution, safe reuse or shader replacement over removing an effect. Retain motion/depth inputs needed by future temporal MetalFX. Shadow/LOD changes need moving-camera and combat checks for pop-in and missing lighting. |
| GAME-04 / medium | `vk_shader_hle.cpp` recognizes copy shader `0xfefebf9f` and uses a host multi-copy dispatch. Its source timings describe another platform, not this M5. The separate `0x3d5ebf4e` copy proof is documented in the Metal runbook. | Game-generated compute work / translation; existing optimization to preserve. | Identify repeated copy/clear shaders by exact module, bindings, offsets and dispatch contract. Compare complete outputs, including untouched regions. Lower a proven operation to a batched host/native operation without changing guest completion order. |
| GAME-05 / medium | Recent Hunter's Dream screenshots show colored edge fringes; `Disable Chromatic Aberration` is available. There is no matched PS4/reference capture establishing a port defect. | Visual effect; observed appearance, attribution open. | Keep paired screenshots with the effect on/off. Record it as an optional image-quality choice unless a renderer mismatch is established; do not claim a speedup from visual preference. |
| GAME-06 / medium | Scene MetalFX currently receives the finished game LDR image at the HUD boundary, including earlier screen effects. Apple recommends spatial inputs with AA and without noise [5]. Colored fringes/high-frequency foliage are visible in the current scene; a reconstruction defect has not been established. | Game post-processing order / upscaler quality; candidate. | Identify noise/grain and AA passes by exact shaders before changing their order. Compare moving foliage, particles and edges with the same exposure/camera. Moving proven noise work after upscaling is a candidate; do not remove motion/depth data or game effects by assumption. |
| PORT-01 / high | `TextureCache::GarbageCollectImages()` decrements `num_deletions` before checking tiled/download safety. Ten skipped oldest images can exhaust a normal pass before eligible images are considered. Earlier local pressure reports recorded no evictions. | Cache/translation; source-confirmed scheduling issue, memory/FPS impact not isolated. | Audit bounded candidate scanning and LRU handling against the upstream fix [4]. Preserve GPU-written tiled-image guards, guest aliasing and ordered writeback. Validate repeated area changes, memory retention and image correctness. |
| PORT-02 / high | Native color-clear gameplay logged at least 6,600 clears across startup/gameplay. Each currently ends rendering and waits for Vulkan/Metal completion. The 20-second measurement had fair thermal pressure. | Native backend; observed operation count and synchronous contract. | Identify clears that can become a subsequent render pass's load action; batch only with a proven dependency/lifetime contract. Count pass endings and waits. The current opt-in path is not a demonstrated speedup. |
| PORT-03 / medium | Current native presentation uses spatial MetalFX. Upstream reports corrected vertical camera-motion reconstruction [4]; our existing camera-motion code needs a separate sign/convention comparison. | Temporal reconstruction; upstream report, not a confirmed local defect. | Before temporal MetalFX, compare camera and object motion for static geometry during stairs, vertical pans and disocclusion; check jitter, depth and Y orientation. Do not infer a defect from the spatial-upscaling run. |
| PORT-04 / high | Native presentation forced a composed 1920x1080 UI frame back to the 1280x720 scene size before upscaling it again. The display override already contains the high-resolution HUD. | Host frame sizing; fixed by allocating at the composed display size after the frame fence completes. | Native logs must retain `1920x1080 -> 1920x1080` during frame preparation. Inspect HUD, menus and resize. MetalFX still scales the complete composite for larger drawables; moving scene upscaling before the HUD remains separate work. |
| PORT-05 / high | Native host conversion, aspect-fit scaling and overlay now share one drawable pass when no final scaler is needed. The latest run recorded 27.3 FPS against 27.5-27.6 FPS Vulkan-host references, 33.33 ms median / 50.02 ms worst-window p99, and no compilation. All comparisons became thermally fair. Earlier native variants had lower throughput; see the runbook for history. | Native backend; fewer passes demonstrated, isolated speedup and stable 30 FPS unproven. Visible overlay and resize passed; native option remains off by default. | Profile the remaining Vulkan raw-frame snapshot and ownership completion while preserving guest lifetime. Repeat alternation under comparable thermals. Filtering now precedes host gamma conversion; full-image and moving-camera comparisons remain open. Private color storage adds 7.9 MiB per 1080p slot only when a final presentation scaler is active. |
| PORT-06 / high | The 1920x1080 HUD exceeds the 1710x961 presentation target, so gameplay MetalFX now runs on the 1280x720 scene before HUD composition. Matched stationary Hunter's Dream runs with the same binary/save/settings recorded 20.9 FPS synchronously versus 26.1 FPS with the host-signaled timeline bridge, about 25% higher throughput. Median interval improved from 50.00 to 33.33 ms; worst-window p99 stayed 50.02 ms. Both sampled nominal pressure and compiled no game shaders/pipelines. The longer final-build run reached 27.7 FPS but became fair and retained the overlay. A separate short UI/resize check varied to 20.2 FPS despite nominal pressure. Both checks are excluded from the matched comparison. | Scene placement and command-thread wait addressed in the opt-in path. Shared-texture lifetime, pending staging/callbacks, three semaphore waits and fallback bytes passed headless checks. Visible overlay/resize and clean shutdown passed. Stable 30 FPS, scanout pacing and equal sustained clocks remain unproven. | Keep `BB_METALFX_SCENE=1 BB_METALFX_SCENE_ASYNC=1` opt-in. Profile remaining GPU submissions and raw-frame ownership waits, then repeat alternating runs under comparable thermals. Audit pixel density/drawable size separately. Use moving-camera/combat checks before broad enablement; worker release timing is not game-thread blocking. |
| WATCH-01 / medium | Upstream reports a particle indirect-dispatch hang around shader `2da7fe60`, following `4ca76892`; it explicitly leaves the cause unresolved [4]. | Particle handling; upstream-only, not reproduced here. | If a freeze recurs, preserve last completed work, indirect arguments, shader IDs and device error. Check counters/barriers before considering shader-loop or dispatch changes. |
| WATCH-02 / medium | The XML contains `Intel Black Tonemap Fix`; upstream reports approximate x86 floating-point differences in game tone mapping [4]. Rosetta relevance is unproven. | Guest CPU color calculations; candidate correctness issue. | Compare matched areas/exposure against a reference before patching. Separate CPU-generated color parameters, sRGB conversions and intentional game lighting. Never treat a darker capture alone as proof. |
| PORT-07 / high | Supported guest draws execute on native Metal. Persistent mirrors track page writes and preserve aliases; eligible read-only draws share one Metal command buffer and Vulkan ownership release. Idle `WaitHostCopies` waits and Liverpool packet-loop signal checks retain a batch when no copies or deferred signals are pending; explicit `WaitDeferredSignals()` calls still flush pending work. The prior same-build A-B-A-B AC sequence averaged 8.21 FPS across two controls and 9.28 FPS across two batched runs (+13.1%), with similar 784–798 draws/frame and nominal pressure. The packet-loop change is not benchmarked. Guest-window p99 was not consistently better (controls 183/200 ms; batched 133/217 ms). Full-run logs show about 24% fewer Metal command buffers; groups average 1.32 draws, maximum six. Native PSO compilation remains outside the existing compile counter. | Repeatable throughput improvement in one tested view; worst-case guest pacing and full-game stability remain unproven. The native path remains far below Vulkan guest rendering in the prior reference scene. | Keep both graphics flags opt-in. Verify another scene and moving/combat workload; improve batching frequency without crossing guest hazards or cache lifetimes. Treat host present-call timings as API measurements, not scanout, and do not claim stable 30 FPS. |
| PORT-08 / high | A mixed native graphics/texture-compute loading run reached `pm4_cmds.h:492 SignalFence`. SPIR-V capture for graphics had incorrectly activated compute even with `BB_METAL_COMPUTE=0`. The flag bug was fixed; isolated native graphics reached gameplay. | Activation bug fixed; texture-compute output/order or packet fault cause remains unresolved. | Keep native guest compute buffer-only until texture outputs and completion order have a Vulkan comparison. Preserve the failed run, last shaders and packet context. Do not skip the assertion, invent a fence address or signal failed GPU work. |
| PORT-09 / high | The native draw path lacked fixed sample-mask/minimum sample-shading handling, and Metal indexed strips always restart at the sentinel. Flat interpolation on interface-block members was also missed by reflection. | Code review findings; focused fixtures now reject these unsupported states before submission and detect flat members for last-vertex rejection. No game-specific visual defect is attributed without a captured case. | Retain Vulkan fallback for these contracts until native lowering/coverage is implemented and output-compared. The page-version build includes these guard fixes; its benchmark is a combined renderer change. |
| PORT-10 / high | Native graphics with async scene MetalFX produced recurring roughly four-second stalls at nominal thermal pressure. A live sample caught graphics blocked creating a command buffer while holding the mutex needed by the scene worker. Preparing 64 graphics commands reproduced the helper blockage in a focused fixture. | Fixed by separate persistent helper/deferred queues with existing Vulkan fence/timeline ordering. Direct/threaded fixtures pass. The final combined gameplay check averaged 8.73 FPS, logged 300 successful pre-HUD upscales, stayed nominal and had no recurring four-second stalls. | Keep both flags opt-in and the failed runs/thread capture. Continue full-game and submission-cost work; this defect was independent of laptop heat. |
| GAME-07 / high | The community debug patch selected an unavailable external shader font branch: context remained null and font drawing wrote through it. Keeping the game's built-in GNM font branch (`0x136D90D`, JE `74`) rendered a readable debug root menu with the supplied literal `adhoc/font/DbgFont14h.ccm` and `.tpf`. | Debug patch/game initialization; branch fixed in `patches/Bloodborne.xml`. Touch contact ID zero also blocked the menu gesture; active IDs now stay nonzero/stable. | Permanent patch/font guards passed 13 tests, booted with native graphics and displayed the readable root categories on the final library in `20261008-151734-673403-debug-menu-font-visual`. The same left-touch also opened the normal gestures panel; this run is UI evidence, not a performance comparison. Never substitute compressed fonts when the guest requests the literal filenames. |
| GAME-08 / medium | bbhost replaces the guest's flush-completion busy loop at analyzed function `0x15d7030` with pause/yield waiting and counters. Its reported stall was measured on other hardware [6]. | Game CPU scheduling; source reference, not a reproduced local bottleneck. | Verify hash/prologue/slide and sample this wait before adapting it. Preserve submitted/done semantics; measure CPU time, thermals and pacing. A source replacement stays x86-64/Rosetta in the current process. |
| GAME-09 / high | bbhost records GX draws before PM4 construction and tracks resource creation, map/discard, release, IDs and writes [6]. Our graphics proof still derives draws from canonical Vulkan/decoded state and clones sparse ranges. | Engine-level native construction and persistent resources; candidate architecture work. | Compare one captured GX draw with the decoded path before replacing it. Publish immutable records before stream tokens, preserve chunk refill/fence order and alias/retirement behavior. Its discrete-GPU PCIe mirror savings do not establish a benefit on unified-memory M5. |

## Souls references and what transfers

1. **Bloodborne / Dark Souls III pacing.** The patch author describes lifting
   the game limiter while using platform flip pacing to retain 30 FPS. This is
   a concrete game-timing lead; his explanation of why the original limiter was
   designed that way is speculation. Console results do not validate our host
   pacing or Apple GPU throughput.
   [illusion's implementation account](https://illusion0001.com/legacy-website/patches/2022/04/18/Fromsoftware-Framepacing/).

2. **Dark Souls / DSfix.** DSfix exposes independent internal/display
   resolution, AO scale, DoF resolution and shader replacement. Its configuration
   distinguishes AO strength from cost and documents gameplay side effects of
   FPS unlocking. Useful inference: tune an identified effect's workload and
   preserve timing semantics. These are interceptor controls, including added
   effects; they are not evidence of identical Bloodborne passes or savings.
   [Author's configuration](https://github.com/PeterTh/dsfix/blob/2a500d24d1965a26d13f1823976a737feecf77d7/DATA/DSfix.ini),
   [render-state implementation](https://github.com/PeterTh/dsfix/blob/2a500d24d1965a26d13f1823976a737feecf77d7/RenderstateManager.cpp).

3. **Dark Souls II / GeDoSaTo.** The DS2 plugin identifies the AA shader,
   captures depth/normal/HDR surfaces and replaces the original FXAA pass while
   restoring render state. Its configuration separates AA, AO blur, DoF and
   bloom. Useful inference: identify an exact Bloodborne pass and replace it
   rather than layering an extra effect over the original. Neither DX9 hooks nor
   DS2 shader IDs transfer directly to PS4 GNM/Metal.
   [Author's DS2 plugin](https://github.com/PeterTh/gedosato/blob/59f1e9706542aa1dcba9e1ad66493afaf7a3b1ba/source/plugins/dark_souls_2.cpp),
   [DS2 settings](https://github.com/PeterTh/gedosato/blob/59f1e9706542aa1dcba9e1ad66493afaf7a3b1ba/pack/config/DarkSoulsII/GeDoSaTo.ini).

4. **Current bbport upstream.** The October 6 changelog reports GC starvation
   fixes, vertical camera-motion correction, a still-unresolved particle hang
   and Intel tone-map differences. It also reports little occlusion-query use
   and no predication in one Cathedral Ward observation: automatic large
   savings from enabling occlusion queries are not established. These findings
   were reported on other hardware. Its Linux dma-buf memory path needs a
   separate Darwin/Metal ownership design.
   [Pinned upstream changelog](https://github.com/deadinside28/bloodborne_pc/blob/4d4d20812c0af23a11c8b8d40f5e06a9054925ef/docs/CHANGES_2026-10-06.md).

5. **MetalFX ordering and dependencies.** Apple's spatial guidance uses a
   tonemapped perceptual input, places particle/noise/UI work after scaling and
   warns that unrelated passes sharing writable resources can lose GPU overlap.
   This supports auditing the exact game post-processing order and our completion
   bridge; it does not prove a particular Bloodborne pass is redundant.
   [Apple's implementation guidance](https://developer.apple.com/videos/play/wwdc2022/10103/).

6. **bbhost engine hooks and source rewrites.** The pinned source supplies GX
   objects/token construction, map/discard/resource tracking, native frame-time
   source with a guest comparison mode, and a yielding flush wait. Windows/Linux
   Vulkan support does not supply a macOS/Metal port. Our next adaptations and
   limits are recorded in [the source comparison](BBHOST_RESEARCH.md), with
   direct links to the inspected files. No bbhost source was imported.

## Game changes and the later patcher

For every game-side change made during the Metal work, record the region/version,
original executable/file hash, expected original bytes, address convention and
slide, exact replacement, dependency/assets, purpose and comparison evidence.
Use private prepared game copies and preserve the original dump/save. The later
patcher must identify the supported input, reject unexpected bytes, handle an
already applied patch, and provide restoration from original data. It is deferred
until after the Metal work as requested; do not turn unverified optimization
ideas or third-party address lists into automatic patches.

## Evidence and evaluation

- Local baseline: [performance investigation](MACOS_PERFORMANCE.md),
  [native Metal stages](MACOS_NATIVE_METAL.md), [benchmark method](MACOS_BENCHMARK.md).
- Clear run: `out/benchmarks/20261006-155126-270220-native-color-clear/`.
  Shader/pass counts are workload descriptions, not proof of redundant work.
- Timing patch / baseline / light-grid runs:
  `20261006-160741-426609-pacing30pp`, `20261006-161119-084093-pacing30-baseline`,
  `20261006-161512-575400-light-grid1080`, under `out/benchmarks/`.
- Native host-pass synchronization comparison:
  `20261006-204515-506098-native-post-swizzle` versus
  `20261006-204806-173874-native-post-vulkan-reference`. Both retained the full
  UI frame and used AC power, original 30 FPS timing and nominal pressure samples.
- Combined native / repeated Vulkan reference:
  `20261006-210117-534512-native-post-combined` and
  `20261006-210505-198243-combined-vulkan-reference`. The native path's slower
  result was retained; passing correctness checks is not a performance win.
- Nominal-pressure light-grid run:
  `20261006-211259-275430-light-grid1080-nominal`, 11 complete windows,
  26.4-27.0 FPS, 33.33 ms medians, worst-window p99 50.02 ms, no compilation,
  clean exit. Its 15-second warmup differed from the reference's 45 seconds;
  no alternating-run or lighting validation has established a benefit.
- Scene MetalFX: `20261006-212923-357699-pre-hud-metalfx`, repeated pre-HUD
  completions, visible Hunter's Dream/HUD and clean exit. Fair pressure flagged
  the 18.2 FPS measurement; reported waits and GPU samples are not a speedup claim.
- Guest graphics: `20261008-123758-997428-native-graphics-isolated`, native draws,
  visible Hunter's Dream and clean exit; fair pressure, 2.61 mean window FPS,
  no isolated performance gain. Mixed failure retained as
  `20261008-123510-597151-native-graphics-debug-font`.
- Async shared-resource graphics: `20261008-150755-813709-native-graphics-async-shared`,
  visible Hunter's Dream, 39,900 native draws, 16,500 async completions, clean
  exit and unchanged source save. Six windows averaged 4.3 FPS with nominal
  pressure, 200–316.68 ms medians and worst-window p99 900.01 ms. The different
  binary/thermals exclude a matched comparison with the synchronous run.
- Persistent mirrors: `20261008-154929-756991-persistent-baseline` versus
  `20261008-155231-164195-persistent-mirrors-async`, same private save/probe/settings,
  35-second measurements, 15-second warmups and nominal pressure. Mean window
  FPS increased 4.80 to 7.05; worst-window p99 fell 283.35 to 183.34 ms. This is
  one sequential comparison between native renderer versions, not a normal
  Vulkan comparison or sustained performance result.
- Page versions: `20261008-160752-619062-persistent-pages-async` averaged 9.00 FPS
  but became thermally fair. `20261008-161952-569604-persistent-pages-cooled`
  used a 60-second cooldown, 20-second measurement and 8-second warmup, averaging
  8.60 FPS with nominal pressure. Visible scene/HUD coverage is recorded in the
  native runbook. These runs exited normally and preserved all source-save hashes.
- Combined graphics/scene MetalFX failure:
  `20261008-174403-381502-persistent-pages-scene-metalfx` averaged 0.55 FPS with
  roughly four-second stalls despite nominal pressure. Its diagnostic repeat,
  `20261008-175118-799721-scene-queue-stall-capture`, also stalled and did not
  produce enough timing windows. Both exited normally. The local thread sample
  `out/macos-native-metal/scene-queue-stall.sample.txt` identifies the dependency;
  `queue-capacity-before-check.log` reproduces it with an expected assertion,
  and `queue-split-{direct,threaded}-check.log` passes after the fix.
- Combined graphics/scene MetalFX after the fix:
  `20261008-175808-693457-queue-split-scene-metalfx`, six windows averaging
  8.73 FPS, 100.00–116.68 ms medians and worst-window p99 150.02 ms, nominal
  pressure throughout. At least 44,400 native draws and 300 actual 720p-to-1080p
  pre-HUD upscales completed, with no scaler fallback. The visible scene/HUD,
  normal exit and unchanged 15 source-save hashes were checked.
- Final Vulkan guest-renderer reference:
  `20261008-225259-787130-queue-split-vulkan-reference`, same GPU/probe/save and
  MetalFX settings as the final native run, with only native graphics flags
  disabled. Six windows averaged 27.15 FPS, with 33.33 ms medians and
  worst-window p99 66.66 ms. Pressure remained nominal, 1,800 scene upscales
  completed and the run exited normally. The source save remained unchanged.
  The native path is still substantially slower; retain the existing default.
- Compare the same save, scene/camera and power state; alternate repeated runs.
  Record nominal/fair/serious/critical pressure, compilation and memory activity.
  Thermally flagged runs can verify correctness but do not isolate a speedup.
- Keep raw captures/game-derived shaders local. Record IDs, settings and results
  here. A visual comparison should include foliage/transparency, particles,
  lighting/shadows, fog, HUD, camera motion and gameplay timing as applicable.
- Promote a candidate only after its actual pass/behavior is identified and
  measured. Record intentional quality reductions explicitly. Reuse existing
  patch selection, profiling and benchmark tooling; defaults change only with
  evidence of correctness and benefit.
