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

## Landed (pass 3, this branch)

- W1 — FF hot/cold constant split + push-block transforms (9e50ea9;
  fixes b48a0a3, 8390a36): VS WORLD/VIEW transforms move off the
  per-draw cbuffer rename onto the push-constant path; PROJECTION,
  TEXTUREn, and lighting/material state stay in the cold FF UBO.
  Result: xform submit_avg_ms 55.76 -> 28.87 (-48%), 7/7 pairs,
  dev-trace-confirmed (bufSub ~8000/frame -> 0); med_ms inconclusive
  this session (loaded-machine confound — concurrent live wine
  session). Design deviation from the approved spec: cold-UBO lights
  are VIEW-baked (d3d9_state.h:84-85 — D3D9Light bakes Position/
  Direction through viewMtx at light-set time), so a VIEW change must
  also dirty the cold block when D3DRS_LIGHTING is on (b48a0a3), and
  the D3DRS_LIGHTING toggle itself must dirty the cold block too, to
  close the off->VIEW-change->on stale-light window (8390a36).
  Semantics are exactly baseline in both fixes; the perf target
  (WORLD-per-draw, hot path only) is untouched.
- W2 — lock fast-path + PrepareDraw texture-mask fast path (4d7b880,
  297b1cd; fix eb2e248): skip the bound-VB-slot walk in LockBuffer for
  direct-mapped buffers (NeedsUpload() is provably always false for
  them, so the FlushBuffer upload path is unreachable), and collapse
  PrepareDraw's texture-upload/mip-gen dirty checks into one combined
  branch — both are no-ops in the common particle-VB / static-atlas
  case. Result: direction confirmed, ~1% RELEASE win in part mode (med
  -0.34%, submit_avg -0.98%, n=7, robust to outlier removal); the
  dev-trace ~10-12% prediction was inflated by rdtsc probe overhead on
  sub-microsecond regions plus feLock/feDraw attribution overlap (Task
  6 spillover into PrepareDraw's VB-upload-mask consumption). Task 7's
  isolated contribution is not isolated / likely small. Cautionary
  note on the fast-path shape: review caught the first cut computing
  the mip-gen mask before the upload that creates it — a MANAGED +
  AUTOGENMIPMAP texture's mip-gen bit, set inside
  UploadManagedTextures itself, was silently dropped by the up-front
  snapshot (a draw would have sampled stale mips). Fixed by gating on
  the raw pre-upload masks and recomputing texturesToGen after the
  upload runs, inside the same branch.
- Layout constants (f0187e2, prep for W1, no behavior change on its
  own): MaxPerStagePushDataSize 32 -> 256, MaxTotalPushDataSize 256 ->
  1376, to hold the FF VS transform push block per stage (64B
  rs-prefix offset + 192B matrices = 256B/stage; total = 64 shared +
  5*256 stages + 32 reserved). pushDataBlockSrcOffset() now returns
  the per-stage region base only; call sites add block.getOffset() for
  the block's position within the region (srcOffset = region base +
  block offset, matching the FE write side's
  computePushDataBlockOffset(index) + offset memcpy).

## Pass 4 measured NEGATIVE result — descriptor construction is NOT the residual

**Claim tested:** that the cost remaining after pass-2's encoder fusion is the
per-restart `MTLRenderPassDescriptor` construction (one autoreleased alloc plus
~33 `objc_msgSend` property writes, ~1024x/frame in `rt` mode). This was the
top-ranked pass-4 candidate.

**What was built:** a hash-indexed (FNV-1a) pool of 16 retained descriptors in
d9mtmetal keyed on the full `WMTRenderPassInfo`, serving repeats with ZERO ObjC
messages. Mutex-guarded; explicit clearing of every field a reused descriptor
would otherwise inherit.

**Mechanism confirmed:** `D9MT_PASS_CACHE_STATS=1` reports
`transitions=122880 hits=122841 misses=39 hit_rate=0.9997` over 120 frames at
`BENCH_RT=256`. The 39 misses are the cold fill. So the work really is being
eliminated, on essentially every restart.

**Wall clock: no effect.** Interleaved ABAB, 4 pairs, 150 frames, on a quiet
host (p99 clustered at 22.2-22.6 ms for 7 of 8 runs, load 8 -> 5.5):

| pair | A med_ms | B med_ms | delta |
|---|---|---|---|
| 1 | 21.136 | 20.820 | -1.50% |
| 2 | 20.854 | 20.675 | -0.86% |
| 3 | 20.183 | 20.758 | +2.85% |
| 4 | 20.640 | 20.977 | +1.63% |

The sign flips; mean paired delta is +0.5%. Resolution on this host is about
+-2%, so the true effect is inside [-2%, +2%].

**Conclusion, and what it redirects:** descriptor construction is not a
meaningful share of the pass-restart cost. Either these property setters are far
cheaper under Rosetta than the ~30ns/message estimate the candidate was sized
with, or the cost genuinely sits where `PERF-ROADMAP` originally said it did —
in the native `renderCommandEncoderWithDescriptor:` / `endEncoding` work itself.
**Do not re-derive this candidate**, and do not size future candidates by
counting `objc_msgSend`s: this is the second time (after pass-3 W2's predicted
10-12% measuring ~1%) that an operation-count model has over-predicted by an
order of magnitude.

Corollary for the remaining pass-4 candidates: those whose entire thesis is
"fewer CPU operations per restart" (the argument-buffer content cache, the
cheap-cleanup bundle, the PSO re-dirty removal) inherit this doubt and should be
measured on a quiet host BEFORE being written, not after.

**Bench-rig caveat that cost most of the measurement time:** this host is a
managed Mac with CrowdStrike (~143% CPU) and Mosyle (~122%) resident, and had a
Virtualization.framework VM at 267% plus concurrent swift builds. At load 20-52
`rt` frame times inflate 2.2x *for both variants*, and an unattended A/B produced
medians ranging 4.5-62 ms. Gate on `sysctl -n vm.loadavg < 6` and check p99
clustering before trusting anything.

## Validation gaps (pass 3) — must close before shipping

- spectest is env-blocked machine-wide this pass (no 3.3.5a client
  reachable anywhere on this machine), so neither W1 nor W2 has
  runnable numeric coverage: FF lighting values (the VIEW-baked cold
  light fix, b48a0a3/8390a36) and the MANAGED+AUTOGENMIPMAP
  upload/mip-regen path (the eb2e248 fix) both rest on verified
  call-chain/state reasoning, not a readback test. consttest,
  depthbias, and resettest are green on every commit in this pass but
  exercise neither path.
- A real-game boot test is mandatory before this payload ships — this
  gates Task 10, not just a recommendation.

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
- Task 7 Option B (closure/chunk-count reduction in the CS enqueue
  path): dev-trace attribution measured csPush at 0.1% of the
  per-batch app-thread submit total (feDraw dominates at 31.4%) — dead
  on arrival, flat before/after Task 6+7, no code changed.
- UnmapTextures() gate in LockBuffer: already one atomic load +
  compare (early-outs on `m_memoryAllocator.MappedMemory() <
  textureMemory`) — inspected per the W2 (Task 6) brief, already
  cheap, left untouched.

## Next candidates, in rough value order (all unproven until benched)

1. Trace-guided WoW capture: bench.c models the workloads synthetically;
   port the apitrace harness (v2/scripts/run-apitrace.sh targets the
   retired v2 driver + CrossOver paths) to the shipped driver and capture
   login-screen / flight-path / particle-storm traces as reference loads.
2. Split s_compileMutex: it serializes spirv-cross codegen AND
   newLibraryWithSource across all 4 PSO workers — area-load bursts
   degrade to ~1 effective compile thread (consttest observed its second
   trivial PSO take 13+ frames). Hold it only around the non-reentrant
   section (or key it per shader).
3. MTLBinaryArchive L2 PSO cache (designed in SHADER-DISK-CACHE-ARCH.md,
   never built): kills the per-launch PSO rebuild in prewarm and the
   first-sight mid-session compiles.
4. Trifan index caching: WoW UI trifans regenerate an index buffer on the
   CPU per draw (d9mt_context.cpp drawEmit); cache per (count, base) or
   convert at bind time.
5. Blit-chain batching for texture-streaming bursts (winemetal already
   accepts chains; encodeBlitCmd sends one crossing per copy today).
6. The 2-3x moonshot stays native arm64 (out-of-process command consumer
   or full port) — every Rosetta-tax trim above is bounded by it.

## Benchmark discipline (hard-won)

- NEVER bench with a background compile running; interleave A/B runs and
  compare medians; the 120 Hz ProMotion vsync pins low draw counts —
  raise BENCH_DRAWS until frame time > 8.33 ms.
- Absolute numbers drift between sessions (display config, thermals,
  background load) — only within-session interleaved A/B is comparable.
- Dev-build traces (D9MT_TRACE=1) are for RELATIVE attribution only.
- Dev-build micro-probe (D9MT_MICRO_BEG/END) segment shares do not
  transfer to RELEASE magnitudes: a dev-trace-predicted ~10-12% win
  measured as ~1% in RELEASE (pass 3, W2) — the probes' own rdtsc
  overhead dominates sub-microsecond regions.
  Use dev traces for relative attribution/routing decisions only, never
  as a magnitude forecast for a RELEASE ship decision.
- On a noisy (shared/loaded) machine, med_ms can invert direction
  outright (pass 3, W1: 5/7 pairs favored HEAD, the median favored base)
  while submit_avg_ms — less exposed to present/vsync scheduling — stays
  monotonic; treat submit_avg_ms as the trustworthy signal on a noisy
  machine and med_ms as corroborating-only until a quiet-session
  recheck. Interleaved same-session A/B (never cross-session absolutes)
  remains the only comparable measurement.
- Pair run order was fixed HEAD-then-base for both pass-3 A/Bs —
  randomize per-pair order in future sessions to rule out a systematic
  first-vs-second-in-pair bias.
- Validate every change with depthbias.exe AND resettest.exe (the Reset
  ladder has caught "safe" dirty-tracking changes before) AND
  consttest.exe (constants staleness) — then a real game boot.
