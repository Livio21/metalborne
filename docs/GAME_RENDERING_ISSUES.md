# Game rendering and handling register

Updated 2026-10-07. Keep this register alongside the native Metal transition.
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
