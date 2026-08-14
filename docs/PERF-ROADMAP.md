# Performance roadmap (WoWSilicon fork)

State as of the `wowsilicon` branch, measured on M5 / macOS 27 under the
WoWSilicon bundled wine runtime (x86_64 under Rosetta 2), with
`test/bench.c` (16k draws/frame, RELEASE builds, interleaved A/B runs).
The driver is CPU-bound on the CS thread; GPU is <1 ms/frame
(V2_ARCHITECTURE.md). Per-draw bridge crossings are already ~zero
(D9MT_BATCH arena); the cost is PE-side, Rosetta-translated plumbing.

## Landed (this branch)

- One ring alloc + one track() per draw in the bind path; per-stage dirty
  granularity; 4-way PSO memo; sampler-heap rebind elision.
  Result: up/tex8 55→65 fps, vb/tex8 62→75 fps at 16k draws; clean path
  unchanged. See the `perf:` commit for full numbers.

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

## Next candidates, in rough value order (all unproven until benched)

1. Trace-guided WoW capture: bench.c models texture-churn UP/VB draws;
   real WoW adds SetTransform-heavy FFP, trifan UI draws, and mid-frame
   texture streaming. Port the apitrace harness (v2/scripts/run-apitrace.sh
   targets the retired v2 driver + CrossOver paths) to the shipped driver
   and capture a login-screen / flight-path trace as the reference load.
2. Deferred clears as load actions: every pending clear forces a render
   pass restart (encodeEmptyRenderPass + full re-dirty + residency reset).
   Merge them into the next real pass's load actions (d9mt_context.cpp
   ~5166; header comment already calls this future work).
3. Push-block micro-costs: elide the full-block memset when the shader's
   blocks + sampler-index writes fully cover pushDataSize (precompute at
   shader build); consider wmtcmd_render_setbytes for <256B blocks to
   skip the ring round-trip entirely.
4. Split s_compileMutex: it serializes spirv-cross codegen AND
   newLibraryWithSource across all 4 PSO workers — area-load bursts
   degrade to ~1 effective compile thread. Hold it only around the
   non-reentrant section (or key it per shader).
5. MTLBinaryArchive L2 PSO cache (designed in SHADER-DISK-CACHE-ARCH.md,
   never built): kills the per-launch PSO rebuild in prewarm and the
   first-sight mid-session compiles.
6. Trifan index caching: WoW UI trifans regenerate an index buffer on the
   CPU per draw (d9mt_context.cpp ~5270/5363); cache per (count, base) or
   convert at bind time.
7. Blit-chain batching for texture-streaming bursts (winemetal already
   accepts chains; encodeBlitCmd sends one crossing per copy today).
8. The 2-3x moonshot stays native arm64 (out-of-process command consumer
   or full port) — every Rosetta-tax trim above is bounded by it.

## Benchmark discipline (hard-won)

- NEVER bench with a background compile running; interleave A/B runs and
  compare medians; the 120 Hz ProMotion vsync pins low draw counts —
  raise BENCH_DRAWS until frame time > 8.33 ms.
- Dev-build traces (D9MT_TRACE=1) are for RELATIVE attribution only.
- Validate every change with depthbias.exe AND resettest.exe (the Reset
  ladder has caught "safe" dirty-tracking changes before), then a real
  game boot.
