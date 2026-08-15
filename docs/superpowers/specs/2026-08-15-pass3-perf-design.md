# d9mt perf pass 3 — per-draw CPU hot paths (xform + part)

Date: 2026-08-15
Branch: `perf/pass-3` (worktree `.d9mt-work/worktrees/pass3`)
Base: `origin/wowsilicon` @ `918e47a` (post payload-v5 pixelFormatView revert)
Status: approved design, pre-implementation

## Context

Pass 1 cut per-draw CPU in the bind path (~20% frame time at 16k draws);
pass 2 landed fused pass transitions, the push-block upload cache, and the
d9vk-parity adapter identity. `docs/PERF-ROADMAP.md` ranks the next
candidates; this wave implements candidates 2 and 3, the two measured
per-draw hot paths, each with an existing bench.c workload and quantified
baseline:

- **xform** — SetTransform(WORLD)+lighting per draw from a static VB (the
  M2-model FFP pattern). Measured: per-draw FF cbuffer rename, `bufSub`
  x8000 = 2 ms/frame on the CS thread + argument-buffer rebuild every draw.
- **part** — dynamic-VB NOOVERWRITE/DISCARD lock+fill batches (the
  particle-system pattern). Measured: app-thread-bound, ~5.7 us per
  Lock+fill+draw batch in dev builds; part mode at 311 ms vs ~65 ms for
  up/vb/rt.

Both changes target the shipped driver: the vendored DXVK 2.7.1 front-end
(`vendor/dxvk/src/d3d9/`) + d9mt Metal backend (`src/d3d9fe/`), built by
`scripts/build-dxvkfe.sh` into `build/d3d9fe.dll`.

## W1 — FF VS constants: hot/cold split, hot slice to push data

### Problem

Every `SetTransform(WORLD)` sets `D3D9DeviceFlag::DirtyFFVertexData`, so
`D3D9DeviceEx::UpdateFixedFunctionVS()` (d3d9_device.cpp ~7993) calls
`m_vsFixedFunction.AllocSlice()` per draw. `D3D9ConstantBuffer::AllocSlice`
(d3d9_constant_buffer.cpp ~77) does a **full storage rename**
(`m_buffer->allocateStorage()` + `EmitCs(ctx->invalidateBuffer)`) every
call. Consequences, measured:

1. CS thread: `bufSub` x8000/frame = ~2 ms.
2. The UBO's GPU address changes every draw, so the VS argument buffer
   rebuilds every draw in the backend's `updateGraphicsShaderResources`.

### Design

Split `D3D9FixedFunctionVS` (d3d9_state.h ~113, ~1.9 KB total) into hot and
cold slices:

- **Hot slice (per-draw): `WorldView`, `NormalMatrix`, `InverseView`**
  (3 x mat4 = 192 B). Moves into a new `StorageClassPushConstant` block in
  the FF VS SPIR-V module (`d3d9_fixed_function.cpp`), replacing the
  `opAccessChain`/`opLoad` reads of those members from the UBO. On the FE
  side, `UpdateFixedFunctionVS()` writes the three matrices via
  `ctx->pushConstants()` into `m_state.pc.constantData` instead of
  `AllocSlice()`. The backend's existing pass-2 push machinery then applies:
  scratch assemble -> memcmp against the persistent per-stage shadow ->
  unchanged pushes skip the ring write entirely; genuinely-changing content
  (xform mode) degrades to a ring-section write + re-bind of the push block
  — with **no rename, no `bufSub`, no AB rebuild** (the push block is bound
  by index, not by address).

- **Cold slice stays a UBO** in `m_vsFixedFunction`'s buffer: `Projection`,
  `TexcoordMatrices[8]`, `ViewportInfo`, `GlobalAmbient`, `Lights[8]`,
  `Material`, `TweenFactor`. The dirty flag splits:
  - `DirtyFFVertexData` (hot) — set by WORLD/VIEW transform changes only.
  - New cold-dirty flag — set by `SetTransform(PROJECTION/TEXTUREn)`,
    `SetLight`/`LightEnable`, `SetMaterial`, viewport/ambient changes.
  The cold UBO re-uploads (rename, as today) only when cold state changes;
  in xform mode that is never per draw. When the cold slice DOES re-upload,
  the hot matrices must NOT be read from it — the shader no longer has
  those members in the UBO block, so there is nothing to keep in sync.

### Push budget

`MaxTotalPushDataSize` = 256 B (vendored `dxvk_limits.h`; a d9mt-internal
limit, not a Metal/Vulkan constraint). Current FF VS push usage (the
render-state block) is ~50-60 B; +192 B hot slice lands at ~250 B — at the
edge. If reflection exceeds 256 B, raise `MaxTotalPushDataSize` to 512
(stack scratch arrays double; negligible cost). The reflected-size check in
`d9mt_shader.cpp` (~339) makes an overflow a loud shader-compile-time
failure, never silent runtime corruption.

### Struct layout discipline

The new push block's SPIR-V member order/offsets must match the FE write
layout byte-for-byte (std140 rules as the existing FF blocks already use).
Implementation must reuse the existing offset-decoration helpers in
`d3d9_fixed_function.cpp` and assert offsets where the pattern allows.

### Explicit non-goals (W1)

- `m_psFixedFunction` / `m_psShared` (FF PS): not on a per-draw hot path;
  left as-is. If bench shows PS-side signal, that's a follow-up.
- `m_vsVertexBlend` (HW vertex blending UBO): WoW does not use FF indexed
  vertex blending on this path; out of scope.
- Programmable-shader constant path (`m_consts[]`): untouched.

## W2 — dynamic-VB lock/draw loop (part mode)

### Problem

part mode is app-thread-bound at ~5.7 us per Lock+fill+draw batch, spread
across `LockBuffer` (d3d9_device.cpp ~5379: device lock, dirty-range
bookkeeping, the bound-VB-slot bitmask walk at ~5436, unconditional
`UnmapTextures()` at ~5511), the app fill, `UnlockBuffer` (~5555), and
`DrawPrimitive -> PrepareDraw` (~7459: full state walk + `EmitCs` closure +
chunk handling). No single obvious culprit — so:

### Step 1 — attribute first (lands before any cut)

Add dev-build-only `D9MT_MICRO` zones around the four segments of the
part-mode batch (Lock / fill / Unlock / DrawPrimitive-incl-PrepareDraw) in
bench.c's part path and the corresponding FE functions. One dev-build bench
run yields the per-segment breakdown. No RELEASE-build instrumentation
(same policy as pass 2: compiled out, zero hot-path cost).

### Step 2 — cut menu, applied in measured order, one commit each

1. **Lock fast-path for dynamic DEFAULT-pool buffers.** When the locked
   buffer is direct-mapped and bound, skip: the bound-slot bitmask walk
   (per-buffer back-link to its slot index makes the `needsUpload` mark
   O(1), or skip entirely when the map mode needs no upload), and gate
   `UnmapTextures()` behind an "any texture currently mapped" flag instead
   of calling it per lock.
2. **CS chunk coalescing.** A Lock(DISCARD)+fill+draw batch emits several
   `EmitCs` closures (`invalidateBuffer`, state, draw); coalesce so the
   batch appends into the open chunk once, reducing closure copies and
   `InjectCsChunk` mutex transitions. No semantic change — CS-thread
   execution order is preserved exactly.
3. **PrepareDraw trim for the dynamic-VB-only case.** Only if attribution
   justifies it: fast path when the sole pending work is `needsUpload` on
   dynamic VBs, skipping texture/mip/sampler mask computation.

### Pivot clause

If attribution shows the cost dominated by `EmitCs` closure allocation
under Rosetta (plausible), pivot to a small closure pool for the hot
closure types instead of the menu above. Any other off-menu result: stop
and re-discuss before coding.

### Explicit non-goals (W2)

- Moving Lock ring management PE-side (the bigger architectural swing;
  candidate for a later wave).
- Touching DISCARD rename semantics (`WaitForResource`, readback rules).
- Changing `FlushBuffer`'s staging copy.

## Error handling and safety rails

- W1 push overflow fails loudly at shader-compile time (reflected size
  check), never silently at runtime. The cold-UBO path is unchanged code
  with unchanged error behavior. `pushConstants` copies synchronously into
  `m_state.pc.constantData` — no lifetime hazards.
- W2 changes only skip or coalesce CPU work; they never reorder
  GPU-visible operations. The lock fast-path activates only under proven
  conditions (direct-mapped, bound, no readback pending); anything else
  falls through to the current path.
- No new env knobs in RELEASE; micro-timers are dev-build only
  (`D9MT_NO_TRACE` still compiles them out).

## Validation

Benchmark discipline per the roadmap (hard-won section): RELEASE builds,
interleaved A/B runs, medians, no background compile running, BENCH_DRAWS
high enough to defeat the 120 Hz vsync pin.

- **bench.c:** `xform` for W1 (target: eliminate the ~2 ms/frame `bufSub`
  x8000 and the per-draw AB rebuild), `part` for W2 (target: measurable
  drop from the 5.7 us/batch attribution baseline). `up`/`vb`/`rt` are
  regression canaries — must stay neutral within noise.
- **Correctness gates after every commit:** `consttest.exe` (constant
  staleness — exactly what W1 touches), `spectest.exe` (FF/lighting
  numerics vs CPU reference, both drivers), `depthbias.exe`,
  `resettest.exe` (Reset ladder). Then a real WoW boot.
- Items that bench neutral get dropped and recorded in the roadmap's
  dead-ends section.

## Rollout

1. W2 step 1 (attribution timers) — informs W2 cut order.
2. W1: shader interface change -> FE plumbing -> bench -> correctness
   gates.
3. W2 menu items, one commit each, benched individually.
4. Roadmap update: "pass 3" section with measured medians and any new dead
   ends.
5. Payload integration (WoWSilicon repo, its own worktree from latest
   remote commit at that time): bump `D9MT_COMMIT` and `PAYLOAD_VERSION`
   in `tools/d9mt/build-payload.sh` — the next payload version is **v6**
   (v5 was the reverted pixelFormatView narrowing) — rebuild, verify
   sha256, update the WoWSilicon Makefile `D9MT_*` pins in one commit.

## Risks and open questions (resolve during planning)

- Exact SPIR-V member layout of the new push block vs the FE struct write
  layout (byte-for-byte match required).
- Whether `DirtyFFVertexData` has other setters that must be audited and
  re-classified hot vs cold (grep all set sites during planning).
- part-mode attribution could point off-menu (see pivot clause).
