# v2 kickoff (WoWSilicon fork)

How V2_ARCHITECTURE.md applies to THIS fork — what is already landed, what the
evidence says, and which cuts come next. Companion to docs/PERF-ROADMAP.md.

## Migration vehicle: in place, on this fork

v2 lands **in-place on this fork**, per V2_ARCHITECTURE §7: de-Vulkanize the
existing `src/d3d9fe` front-end one validated seam at a time, converging toward
the §6 layout. The `v2/` directory is the **upstream author's separate
testbed** (fresh DXVK frontend targeting GTA IV under CrossOver — see
v2/ROADMAP.md's paths and exit criteria). It is a useful reference for what a
cut looks like when it works (e.g. its per-frame `useResource` dedupe,
`59c9e09`), but it is **not** the migration vehicle for WoWSilicon: our
workload, runtime (bundled wine, x86_64 under Rosetta), bench rig, and shipped
payload all live on the fork's `src/` tree.

## §7 phases mapped onto what pass 2/3 already landed

Landed work (see PERF-ROADMAP.md "Landed (pass 2/3)") already covers slices of
several §7 phases, so the phase list is partially done rather than pristine:

| §7 phase | Status on this fork |
|---|---|
| 0. Measure app-thread side | **Done** — pass-3 rdtsc attribution (below) |
| 1. POD staging | **This branch** — per-cmdlist upload ring converted |
| 2. Residency set | Partial — `markResident` open-addressing set dedupes per encoder; per-frame dedupe not done |
| 3. Frame-scoped ownership | Not started (largest, behind a flag) |
| 4. Compact PSO key | Partial — 4-way PSO memo (pass 2) kills the common lookup; key itself still Vulkan-shaped |
| 5. POD command packets | Partial by construction — render commands are already POD `wmtcmd` structs; `EmitCs` lambdas remain |
| 6. Devirtualize draw path | Not started |
| 7. File split (§6 layout) | Not started — `d9mt_context.cpp` is still the monolith |

Adjacent pass-2/3 wins that reduce what v2 has left to claim:
- **Fused encoder transitions** (pass 2): 6-7 winemetal crossings per pass
  restart collapsed to one; rt mode 26.8 → 22.9 ms at 256 round-trips/frame.
- **Push-block upload cache** (pass 2): content shadow + memcmp skips
  byte-identical push re-uploads; neutral on up/vb/xform, part unaffected
  (app-thread-bound).
- **FF hot/cold constant split** (pass 3, W1): WORLD/VIEW transforms moved to
  the push path; xform submit_avg 55.76 → 28.87 ms (-48%, 7/7 pairs).

## Phase-0 evidence already measured

- **App-thread FE cost dominates the particle/model complaints.** part mode is
  app-thread-bound at **~5.7 us per Lock+fill+draw batch under Rosetta**
  (pass-3 attribution, docs/superpowers/specs/2026-08-15-pass3-perf-design.md);
  the CS thread and GPU are not the bottleneck for those modes.
- **feDraw is ~31% of the per-batch app-thread submit total** (31.4%,
  PERF-ROADMAP.md "Measured dead ends", Task 7 attribution) — the draw-side
  plumbing, not the CS enqueue (csPush measured 0.1%, dead on arrival).
- **Pass-4 negative result:** encoder-descriptor construction is **not** the
  pass-restart residual. A 99.97%-hit descriptor cache moved wall clock by
  ±2% (= host resolution). Corollary: do not size candidates by counting
  operations/`objc_msgSend`s — that model has over-predicted by 10x twice.

## Validation gate — every phase, this fork

Every v2 seam must pass, on this fork, before merge:

1. **Correctness triple:** `depthbias.exe` + `resettest.exe` + `consttest.exe`
   green (the Reset ladder has caught "safe" dirty-tracking changes before).
2. **Perf:** interleaved ABAB bench, `rt`/`part`/`xform` modes, under
   `tools/bench-wowsilicon.sh`, on a **quiet host**: loadavg < 6 and p99
   within ~5% of median, else the run is contaminated (PERF-ROADMAP bench
   discipline; this managed Mac inflates frames 2.2x under load).
3. **Standing policy:** a **real WoW 3.3.5a boot test is required before
   shipping** any payload containing the change — unless the user explicitly
   signs off the gap for that release (as tracked in PERF-ROADMAP "Validation
   gaps").

Behavior must be bit-identical for structural cuts (same commands, same bytes);
anything touching dirty/Reset logic escalates to the full §8 discipline.

## Next v2 cuts, ranked

1. **Phase 1 — POD staging, upload-ring slice** *(this branch)*. Evidence:
   direct — the ring's `DxvkBufferSlice` return copies an `Rc<DxvkBuffer>`
   (2 atomic ops per alloc/destroy) on a path already measured as part of the
   per-draw submit cost; the pass-2 packed-slice change ("ONE ring allocation
   + ONE track()") already showed the alloc/track collapse direction is right.
   Small, behavior-identical, bounded blast radius.
2. **Phase 2 — per-frame residency dedupe.** Evidence: indirect — the v2
   testbed's per-frame `useResource` dedupe was worth landing there
   (`59c9e09`), and `markResident` runs per AB rebuild here; but per-encoder
   dedupe already exists on this fork, so the residual is **unmeasured**.
   Measure before writing (pass-4 corollary applies).
3. **Phase 4 — compact PSO key.** Evidence: weak — the PSO memo already
   short-circuits the common lookup, and `buildPipeline`-class costs measured
   ~0 steady-state on the v2 testbed. Only worth it if attribution shows the
   full-state FNV hash + mutex + map probe on memo misses is real frame time.

Phases 3/5/6 (frame-scoped ownership, POD packets end-to-end, devirtualize)
stay behind the same rule: attribution first, one seam at a time, flag-guarded
where dirty/Reset logic is touched.
