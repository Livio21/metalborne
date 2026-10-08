# bbhost as a reference for native Metal

Reviewed 2026-10-08 at
[`7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e`](https://github.com/droogie/bbhost/tree/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e).
This review used the rendering/decompilation/build docs and the source files
linked below. No bbhost code has been imported into Metalborne, and bbhost has
not been built or benchmarked on this Mac.

## What it provides

bbhost's documented targets are x86-64 Linux and Windows, with a Vulkan
renderer. It is a useful source of game-engine hooks and selected source
reimplementations, rather than an existing Apple Silicon/Metal backend.
The current Metalborne architecture remains bbport's x86-64 runtime under
Rosetta with native Metal graphics being added incrementally.
[Build requirements](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/docs/building.md).

| Mechanism inspected | Relevance to Metalborne | Limit or prerequisite |
| --- | --- | --- |
| GX draw hooks capture engine objects and put a small draw token into the Gnm stream. The command processor consumes the token at the original draw position. | Candidate route from game intent to Metal draw/resource construction, avoiding reconstruction of every graphics draw from registers. | Compute, clears, fences and labels still need ordered handling. We cannot submit hooked draws immediately and ignore their position in the stream. |
| Resource creation/map/release hooks track holders, object IDs, memory and write sequences. | Information needed to keep persistent Metal resources and distinguish changed or reused guest allocations. | The renderer still uses guest addresses and write tracking. Hooks alone do not replace the allocation/aliasing/completion contract. |
| Device-local vertex/index mirrors cache unchanged pages and invalidate them on CPU/command-processor writes. GPU-written pages take the imported path. | The invalidation rules are useful for replacing per-draw sparse clones with persistent native buffer storage. | The cited performance rationale is PCIe reads on discrete GPUs. M5 uses unified memory; duplicating that mirror scheme is not an established optimization here. |
| The frame-time manager is rewritten as C++ with a guest comparison mode. | A source reference for preserving 30 FPS timing/history while improving host pacing. | Preserve fixed-step simulation, background behavior and late-frame handling. A 60 FPS patch set is outside the current target. |
| The render flush wait uses a short pause-spin followed by yielding, with wait counters. | A game-side candidate for avoiding an idle core spinning while the command processor finishes. | Confirm the function, executable bytes and hot-path cost here before replacing it. A yield changes scheduling; it is not proof of faster GPU work. |
| Typed SPIR-V lifting and one-draw capture/replay tools. | Candidates for simpler MSL and isolated shader/output comparisons. | Translated shader equivalence and Apple GPU performance must be measured; valid SPIR-V alone proves neither. |

Sources: [rendering architecture](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/docs/rendering.md),
[GX token implementation](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/hle/gx_trace.cpp#L1866),
[resource hooks](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/engine/gx_resources.cpp),
[buffer mirrors](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/host/buffer_shadow.cpp),
[frame-time implementation](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/engine/frame_rate.cpp),
[flush wait](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/decomp/gx_flush_wait.cpp).

## Concrete findings to preserve

The GX token implementation publishes the draw record before writing the token.
It uses ring slots with an overflow map, so a busy slot does not lose a draw.
Its refill callback can submit a chunk and wait for the command processor;
holding a lock needed by the consumer across refill would deadlock. A direct
Metal adaptation needs the same publication, refill and retirement guarantees.
The token is a four-dword NOP packet carrying magic and a 64-bit record ID.
Its exact representation is bbhost-specific, not a format already implemented
in our PM4 decoder.
[Token publication/consumption](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/hle/gx_trace.cpp#L1866).

The resource registry watches buffer creation at `0x2565c10`, texture creation
at `0x2565f90`/`0x2566300`, map at `0x2569c30`, and buffer release at
`0x256d690`. These are addresses in bbhost's analysis convention, not validated
patch offsets for our private executable. Its map hook re-reads the backing
memory because discard/rename can move it. New holder IDs and write sequences
prevent an address-only interpretation of object lifetime. Texture view extents
come from the engine's mip/slice descriptions.
[Registry implementation](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/engine/gx_resources.cpp#L1).

The frame-time source has explicit guest/ours/compare modes. Comparison starts
from the same pre-update object and checks the fields written by the update,
using the guest's release time. This is a useful method for a game-side timing
change: compare behavior independently of a faster/slower clock sample. The
source also separates fixed frame durations from per-frame quantities changed
by its optional 60 FPS work. We continue to target 30 FPS.
[Frame-time source](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/src/engine/frame_rate.cpp#L542),
[decompilation method](https://github.com/droogie/bbhost/blob/7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e/docs/decomp.md).

## Next use in this port

1. Extend the compiled async shared-resource graphics path beyond its passing
   direct/threaded Vulkan comparison fixture. It removes caller-side completion
   waits only for already shared resources; it does not eliminate per-draw
   submissions or make sparse clone draws asynchronous.
2. Use the resource hooks/layouts as references to verify stable vertex/index
   ranges, dynamic constants, aliases and map/discard behavior in our dump.
   Replace pooled per-draw sparse clones with persistent native ownership only
   after those writes and retirements are accounted for.
3. Capture one GX draw alongside the existing decoded draw and compare its
   shaders, bindings, constants and render state before replacing construction.
   Reuse bbport's draw preparation/cache work where it already gives the same
   result. Keep labels/fences ordered while migrating clears/copies/compute.
4. Profile the identified guest flush-wait and frame-time functions. Any game
   rewrite needs version/hash, expected original bytes, ABI/state and output
   evidence recorded in the rendering register. Keep changes to private prepared
   copies; the reproducible patcher remains work for after the Metal transition.

These steps are engineering inferences from the inspected code and our slow
native graphics proof, not claimed bbhost performance results on Apple Silicon.
The existing [rendering register](GAME_RENDERING_ISSUES.md) tracks the candidates
and [native runbook](MACOS_NATIVE_METAL.md) distinguishes compiled checks from
gameplay/performance coverage. bbhost's repository uses GPL v3; this review does not change
Metalborne's license or make it a bbhost fork.
