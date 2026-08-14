# Performance roadmap (WoWSilicon fork)

State as of the `wowsilicon` branch, measured on M5 / macOS 27 under the
WoWSilicon bundled wine runtime (x86_64 under Rosetta 2), with
`test/bench.c` (RELEASE builds, interleaved A/B runs, medians). The
driver is CPU-bound; GPU is <1 ms/frame (V2_ARCHITECTURE.md). Per-draw
bridge crossings are ~zero (D9MT_BATCH arena) and pass-restart crossings
are fused (d9mtmetal pass transition); the remaining cost is PE-side,
Rosetta-translated plumbing.

## Workload model (pass 2)

bench.c modes map to the user-visible WoW complaints:
- `up`/`vb`: texture-churn quad draws (UI/world mix) — general draw cost
- `xform`: SetTransform(WORLD)+lighting per draw from a static VB — the
  M2-model FFP pattern ("many models loaded" complaint)
- `part`:  dynamic-VB NOOVERWRITE/DISCARD lock+fill batches, alpha
  blended, z-write off — the particle-system pattern
- `rt`:    per unit: switch to a small RT, clear, draw, switch back,
  sample it in the main pass — the shadow-blob/projected-texture pattern

Pass-2 baseline (payload v3, 16k draws / 256 rt round-trips):
  up 64.8ms | vb 63.4ms | part 311ms(!! app-thread) | rt 67.8ms
  (rt: 265us per round-trip, ~75% of it startRenderPass; dev-build trace:
  startRP x512 = 16.6ms of a 22.5ms frame)

## Landed (pass 2, this branch)

- Fused encoder transitions: endEncoding+release+pool+create+retain per
  pass restart (6-7 winemetal crossings) collapse into ONE d9mtmetal
  crossing (D9MT_FUNC_PASS_TRANSITION); deferred-clear-only passes fuse
  end-old+begin+end-new the same way. Empirically verified winemetal
  handles are raw ObjC pointers (probe: AGXG17GFamilyCommandBuffer), so
  native-side encoder create/end interoperates with winemetal-encoded
  command chains.
  Result: rt mode median 26.8 -> 22.9 ms at 256 round-trips/frame (~15% frame time,
  tight ABAB interleave; the residual cost is the native encoder
  create/end work itself, not crossings).
- Push-block upload cache: per-stage content shadow + memcmp; unchanged
  push bytes skip the ring section + encode entirely; across a pass
  restart they degrade to a single re-bind of the still-live slice
  (gen-guarded against cmdlist recycling, epoch-guarded against encoder
  handle reuse). Push zeroing now covers only reflection-computed
  uncovered ranges instead of a full memset; the set-0 AB skips its
  memset when every slot is statically covered (null bindings write 0).
  Result: up/vb/xform neutral within measurement noise on a loaded host
  (the saved ring/encode work trades against the assemble+memcmp);
  part unchanged — it is app-thread-bound (see next candidates).
- Adapter identity parity with d9vk (correctness, not perf): DXVK 2.7
  hides the Apple vendor behind an AMD RX 6700 XT identity; WoW keys
  vendor-specific feature paths (terrain specular among them) off the
  vendor id, so the d9vk->d9mt swap changed what the game itself chose
  to render ("sun reflection way too glossy" report). d9mt now defaults
  to the d9vk-visible identity (0x106b + Metal device name);
  D9MT_ADAPTER_SPOOF=amd restores upstream hiding; dxvk.conf wins.

## Verified-faithful (pass 2 tests; keep green)

- test/consttest.c: PS/VS constant updates, texture swaps, blend-toggle
  PSO swaps, and mid-scene clear restarts all propagate (readback).
- test/spectest.c: the REAL 3.3.5 terrain sun-specular shader pair
  (vs20 permutation 4 + terrain1 ps20, extracted via
  tools/extract-wow-shaders.py — blobs never committed) renders
  numerically faithful to a CPU D3D reference across 4 exponents x 8
  N.H values under BOTH d9mt and the retired d9vk payload — including
  pow(x,0)=1 (strictPow) and the N.H=0 edge under fast-math MSL.
- test/capstest.c: caps diff d9mt vs d9vk is now identity-only
  (SrcBlendCaps/DestBlendCaps advertise 2 extra D3D9Ex bits under d9vk;
  WoW-irrelevant).

## Measured dead ends (do not retry without new evidence)

- `thread_local` memos for the side-state lookups: mingw PE lowers TLS to
  emutls — micro-probe `look` went 1.55 ms → 4.54 ms per 112k calls.
  The uncontended mutex+hash costs ~14 ns/call; it is NOT a bottleneck.
- Metal 4 (probed empirically): the full MTL4 class set, MTL4 command
  queues/compiler, argument tables AND MTLResidencySet respond under
  Rosetta x86_64 on macOS 27 — availability is NOT the blocker. But the
  measured cost is CS-thread state processing, not Metal encode (the
  decode loop native-side and the GPU are both far from saturated), so
  adopting MTL4 command encoding moves nothing today. Revisit only after
  the PE-side plumbing is POD (V2 phases) or for a native-arm64 consumer
  process. Note the macOS 15 floor of WoWSilicon: any MTL4 use must be
  runtime-gated.
- WINEDLLPATH does NOT reroute unixlib .so resolution under the bundled
  runtime — pair a fresh d9mtmetal.so with its PE dll by replacing them
  inside a COPY of the runtime tree. Version skew is unbounded UB: the
  wine unixlib dispatch has no bounds check, so a PE calling a function
  the .so lacks executes whatever follows the call table (we lost hours
  to "black renders" and phantom asserts from exactly this).

## Next candidates, in rough value order (all unproven until benched)

1. Trace-guided WoW capture: bench.c models the workloads synthetically;
   port the apitrace harness (v2/scripts/run-apitrace.sh targets the
   retired v2 driver + CrossOver paths) to the shipped driver and capture
   login-screen / flight-path / particle-storm traces as reference loads.
2. App-thread per-call cost (part mode is app-thread-bound: ~5.7us per
   Lock+fill+draw batch in dev builds): the FE call surface itself under
   Rosetta. Candidates: slim PrepareDraw for the dynamic-VB path, batch
   the CS chunk enqueues, or move the Lock ring management PE-side.
3. Per-draw FF cbuffer rename (xform mode: bufSub x8000 = 2ms/frame CS +
   AB rebuild every draw): write FF constants into the packed slice
   directly (skip the DxvkBuffer rename), or delta-upload transforms.
4. Split s_compileMutex: it serializes spirv-cross codegen AND
   newLibraryWithSource across all 4 PSO workers — area-load bursts
   degrade to ~1 effective compile thread (consttest observed its second
   trivial PSO take 13+ frames). Hold it only around the non-reentrant
   section (or key it per shader).
5. MTLBinaryArchive L2 PSO cache (designed in SHADER-DISK-CACHE-ARCH.md,
   never built): kills the per-launch PSO rebuild in prewarm and the
   first-sight mid-session compiles.
6. Trifan index caching: WoW UI trifans regenerate an index buffer on the
   CPU per draw (d9mt_context.cpp drawEmit); cache per (count, base) or
   convert at bind time.
7. Blit-chain batching for texture-streaming bursts (winemetal already
   accepts chains; encodeBlitCmd sends one crossing per copy today).
8. The 2-3x moonshot stays native arm64 (out-of-process command consumer
   or full port) — every Rosetta-tax trim above is bounded by it.

## Benchmark discipline (hard-won)

- NEVER bench with a background compile running; interleave A/B runs and
  compare medians; the 120 Hz ProMotion vsync pins low draw counts —
  raise BENCH_DRAWS until frame time > 8.33 ms.
- Absolute numbers drift between sessions (display config, thermals,
  background load) — only within-session interleaved A/B is comparable.
- Dev-build traces (D9MT_TRACE=1) are for RELATIVE attribution only.
- Validate every change with depthbias.exe AND resettest.exe (the Reset
  ladder has caught "safe" dirty-tracking changes before) AND
  consttest.exe (constants staleness) — then a real game boot.
