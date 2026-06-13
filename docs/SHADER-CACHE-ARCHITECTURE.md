# d9mt Compiled-Shader Cache — Layering & Abstraction Architecture

Status: proposal. Scope: where shader-compile responsibilities live, the
compile-backend abstraction, the PE↔unixlib ABI, and how it stays
backend-agnostic (CLI today; airconv / Metal Shader Converter / Metal 4 later).

---

## 0. First principles

Three forces shape every decision:

1. **The expensive thing must run out-of-the-frame, out-of-the-Metal-lock.**
   `newLibraryWithSource` holds an internal Metal compiler lock and stutters.
   The *only* call we trust on the frame path is `newLibraryWithData` (load a
   ready `.metallib`, no source compiler). So the architecture's job is to make
   "produce the `.metallib`" a separable, swappable, cacheable step that the hot
   path never touches on a cache hit.

2. **The compile backend is a moving target; everything else is not.** The
   *artifact* (a `.metallib` blob keyed by a content hash) is stable forever.
   The *producer* of that blob will change 3-4 times over the project's life.
   Therefore the cache, the key, and the load are core; the producer is a leaf
   plugin. Lock-in lives wherever a producer-specific concept (CLI argv, DXIL,
   subprocess) leaks past the leaf.

3. **The layer that owns a resource owns its failures.** macOS fs, subprocess,
   and Metal all live arm64-native. The PE side must never learn that a backend
   spawns a process or that a toolchain is missing — it asks for bytes and gets
   bytes-or-nothing.

---

## 1. The three layers and who owns what

```
┌─────────────────────────────────────────────────────────────────────────┐
│ LAYER A — d3d9fe.dll  (x86 PE, DXVK front-end + d9mt backend)            │
│                                                                           │
│   DXSO → DXVK IR → SPIR-V → spirv-cross → MSL text   [stays here]        │
│   reflection (AB ids, push blocks, samplers, spec constants) [stays]     │
│   ShaderKey = stable content hash over (MSL bytes + backend id + target) │
│   PsoWorkers async pool, PSO cache, FunctionCache       [stays here]     │
│                                                                           │
│   Owns: WHAT to compile (MSL + identity). NOT how, NOT where on disk.    │
│   Calls down through ONE opaque verb: "give me a library for this key".  │
└───────────────────────────────┬───────────────────────────────────────────┘
                                 │  __wine_unix_call, fixed-width u64 structs
┌───────────────────────────────▼───────────────────────────────────────────┐
│ LAYER B — winemetal bridge  (DXMT-derived, DO NOT FORK HEAVILY)          │
│                                                                           │
│   Pure, dumb marshalling of existing primitives we already inherited:    │
│   MTLDevice_newLibrary(data), DispatchData, CacheReader/CacheWriter,     │
│   MTLBinaryArchive_serialize, newRenderPipelineState(binary_archives).   │
│                                                                           │
│   Owns: NOTHING new. We add ZERO d9mt policy here. It is a syscall ABI.  │
│   Rationale: forking winemetal couples us to upstream DXMT forever.      │
└───────────────────────────────┬───────────────────────────────────────────┘
                                 │  same ABI; OUR new entry table (d9mtmetal)
┌───────────────────────────────▼───────────────────────────────────────────┐
│ LAYER C — d9mtmetal unixlib  (arm64 native: Metal + fs + subprocess)    │
│                                                                           │
│   The CompileBackend implementations live here and ONLY here:           │
│     • CliBackend     → spawn `xcrun metal`, read .metallib bytes         │
│     • MscBackend     → libmetalirconverter in-proc (future)             │
│     • AirconvBackend → LLVM in-proc DXBC→AIR    (future)                │
│     • SourceBackend  → newLibraryWithSource (the live fallback)          │
│   The on-disk cache (metallib blobs keyed by ShaderKey).                 │
│   metallib→DispatchData→newLibraryWithData load.                         │
│                                                                           │
│   Owns: HOW it's compiled, WHERE the cache lives, subprocess lifetime,   │
│          toolchain probing, all failure recovery + fallback selection.   │
└───────────────────────────────────────────────────────────────────────────┘
```

### Boundary justifications (the load-bearing decisions)

- **MSL generation stays in Layer A.** It is DXVK/spirv-cross work, x86, and
  produces *reflection that the PE side must consume anyway* (AB ids, push
  blocks). Splitting MSL gen from its reflection would mean shipping reflection
  back across the ABI for no reason. MSL bytes are the natural hand-off unit.

- **The disk cache lives in Layer C, native-side, NOT in the PE.** Reason: the
  cache's value type is a `.metallib` and its consumer is `newLibraryWithData`,
  both native. If the PE owned the cache it would have to pull blob bytes across
  the ABI on every hit just to push them back down to load — a pointless round
  trip of the largest payload in the system. **Bytes stay native; only keys and
  handles cross.** This is the single most important boundary rule.

- **Subprocess spawn lives in Layer C and is invisible above it.** The PE has no
  business knowing a backend shells out. `fork/exec`, temp-file plumbing, and
  `xcrun` discovery are Layer-C-private. When we swap CLI→in-proc MSC, *nothing
  above the backend interface changes* — that is the whole point.

- **winemetal (Layer B) gains nothing.** We already inherited everything we need
  there (CacheReader/Writer, DispatchData, newLibrary, BinaryArchive). New d9mt
  verbs go in the *d9mtmetal* table (our own `__wine_unix_call_funcs`), never by
  editing winemetal. This keeps the DXMT-derived file a clean vendored drop.

---

## 2. The compile-backend abstraction

A single interface, defined native-side, that decouples "loadable library for
this shader" from how it's produced.

```
interface CompileBackend {
    BackendId   id();                       // stable enum, part of the cache key
    InputKind   accepts();                  // bitmask: {MSL_TEXT, SPIRV, DXBC}
    bool        available();                // toolchain/lib present right now?

    // The one verb. Pure: identical input + same backend id ⇒ identical bytes.
    Result<MetallibBytes> compile(
        InputBlob   source,                 // MSL text today; SPIR-V/DXBC later
        TargetEnv   target);                // metal lang version, fast-math, os
}
```

- **Input contract: a tagged blob, not "MSL".** The interface accepts an
  `InputBlob{ kind, ptr, len }`. Today every backend is fed `MSL_TEXT`. When MSC
  (eats DXIL, not SPIR-V) or airconv (eats DXBC) arrive, the *front-end already
  has SPIR-V and even DXBC upstream of spirv-cross* — so a future backend
  advertises `accepts() = SPIRV` and Layer A hands it the earlier artifact
  instead of running spirv-cross at all. The `kind` tag is what lets the chain
  shorten without the interface changing. We never assume MSL is the only input.

- **Output contract: `.metallib` bytes. Always. Never a live MTLLibrary
  handle.** The backend's job ends at "bytes on the native heap." Turning bytes
  into an `MTLLibrary` is a *separate, trivial, backend-independent* step
  (`DispatchData` + `newLibraryWithData`). Keeping these apart means: (a) the
  same load path serves cache hits and fresh compiles, and (b) `SourceBackend`
  (the live fallback) is the *one* exception that returns a handle directly — it
  signals `MetallibBytes::live(handle)`, and the load step short-circuits. One
  union type models "bytes I must load" vs "already a library."

- **Backend selection: a priority chain, resolved once at init, overridable by
  env.** Native-side registry holds an ordered list. Default order encodes our
  preference (in-proc > subprocess > live source):
  `[MscBackend, AirconvBackend, CliBackend, SourceBackend]`. At startup each is
  probed via `available()`; the first available *that accepts the input we can
  supply* wins as the "producer," with `SourceBackend` always last as the
  guaranteed floor. `D9MT_SHADER_BACKEND=cli|msc|airconv|source` pins one for
  bring-up and A/B. **The PE never names a backend** — it passes the input kind
  it happens to have, and Layer C picks.

### Rejected designs

- ✗ *Backend interface that returns an `MTLLibrary` handle.* Couples produce to
  load, kills the unified cache-hit path, and forces every future in-proc
  backend to also own DispatchData plumbing.
- ✗ *Backend selection in the PE via a function pointer / vtable across the ABI.*
  You can't pass C++ vtables over `__wine_unix_call`, and it would leak backend
  identity (and CLI-ness) upward. Selection is a native-only concern.
- ✗ *One mega-backend with `if (cli) … else if (msc) …`.* That is the lock-in we
  are designing against. Each producer is its own object behind `available()`.

---

## 3. PE↔unixlib ABI shape

The current ABI is one fixed-width u64 struct per verb in a numbered table.
That's fine; the failure mode to avoid is *one new unix func per
(backend × stage × variation)*. We collapse to **two stable verbs plus a tiny
opcode dispatcher**, keeping bytes native.

```
enum d9mt_unix_func {
  D9MT_FUNC_LIBRARY_FOR_KEY = 0,   // NEW: key → MTLLibrary, all caching native
  D9MT_FUNC_NEW_RENDER_PSO  = 1,   // existing
  D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE = 2,  // demoted to internal fallback only
  D9MT_FUNC_COUNT
};

struct d9mt_library_params {     // all u64 / fixed-width; 32==64-bit layout
  uint64_t device;               // in
  uint64_t key_ptr;              // in  : ShaderKey bytes (content hash digest)
  uint64_t key_len;              // in
  uint64_t source_ptr;          // in  : MSL/SPIR-V/DXBC blob (only read on MISS)
  uint64_t source_len;           // in
  uint32_t source_kind;          // in  : InputKind (MSL_TEXT=0, SPIRV, DXBC)
  uint32_t target_flags;         // in  : msl version, fast-math, os baseline
  uint64_t ret_library;          // out : retained MTLLibrary, or 0
  uint64_t ret_status;           // out : enum {HIT, COMPILED, FELL_BACK, FAILED}
  uint64_t ret_error;            // out : retained NSError or 0
};
```

**Why this shape:**

- **The PE passes a key *and* the source, but the source is only touched on a
  miss.** One round trip resolves hit-load, miss-compile-write-load, and
  fallback uniformly. No "check then compile" two-call dance across the ABI
  (which would race and double the marshalling).
- **Bytes flow down only; only a handle flows up.** `ret_library` is the only
  large-ish thing returned, and it's an opaque handle. The `.metallib` never
  crosses. Honors §1's central rule.
- **One verb absorbs all future backends.** Swapping CLI→MSC changes which
  native object services `D9MT_FUNC_LIBRARY_FOR_KEY`; the struct, the opcode, and
  every PE call site are untouched. `source_kind` already lets the PE feed
  SPIR-V/DXBC the day a backend wants it.
- **`ret_status` makes degradation observable without changing control flow.**
  The PE logs/telemetries it; it never branches on backend identity.
- We **keep the existing CacheReader/CacheWriter/DispatchData** winemetal
  primitives as the implementation of the native cache — no new cache ABI is
  invented, satisfying "don't fork winemetal."

---

## 4. Two compile stages: library vs pipeline

There are genuinely two compiles, and they must be modeled **separately,
because their cache keys and their backends differ**:

| Stage | Input → Output | Key | Backend | Mechanism |
|-------|----------------|-----|---------|-----------|
| **Library** | MSL/IR → `.metallib` | content hash of source+target | *pluggable* (CLI/MSC/airconv/source) | §2 CompileBackend |
| **Pipeline** | functions + render state → PSO | shader pair + packed state (existing `PsoKey`) | *fixed* — only Metal can build a PSO | `MTLBinaryArchive` |

Treating them uniformly would be a category error: a PSO can *only* be produced
by Metal's own pipeline compiler, so there is nothing to make pluggable at the
pipeline stage. What *is* cacheable there is Metal's own `MTLBinaryArchive`: feed
`binary_archives_for_lookup` to `newRenderPipelineState` (hit ⇒ no recompile),
and `binary_archive_for_serialization` to record misses. winemetal already
exposes both fields — so the pipeline cache is a **separate, fixed module** that
reuses existing ABI, while only the *library* stage carries the backend
abstraction.

**Spec-constant specialization (`newFunctionWithConstants`) belongs to neither
producer — it's a third axis, the function layer.** The current
`FunctionCache` (keyed by spec values) is correct and stays. The clean model is
three nested caches:

```
ShaderKey ──► Library (.metallib, backend-produced, disk-cached)   [stage 1]
                 └─► (specValues) ──► Function (newFunctionWithConstants) [layer]
                          └─► (PsoKey) ──► PSO (BinaryArchive-cached)  [stage 2]
```

Specialization is deliberately kept **above** the library cache and **below** the
PSO cache: one `.metallib` serves all spec variants (constants are resolved at
`newFunction` time, cheap, in-proc), so we don't multiply disk artifacts per
spec value. This is why the library cache key is *the MSL*, not the MSL+spec.

---

## 5. Failure & fallback as a first-class concern

Fallback is expressed entirely inside Layer C, behind the single verb, so no
caller ever learns a backend was missing.

```
library_for_key(p):
    if hit = cache.get(p.key):              return load(hit)        → HIT
    backend = registry.first_available_accepting(p.source_kind)
    if backend and backend != SourceBackend:
        if bytes = backend.compile(source, target):
            cache.put(p.key, bytes)          // write-through, native
            return load(bytes)               → COMPILED
        // else fall through — compile failed, try the floor
    lib = SourceBackend.compile_live(source) // newLibraryWithSource, in-proc
    return lib ? FELL_BACK : FAILED
```

- **`SourceBackend` is the guaranteed floor**, always `available()`. Worst case
  we are exactly as good as today (a stutter), never worse. This makes shipping
  any new backend strictly safe: if it's absent or breaks, we silently degrade.
- **Negative results are cached too** (a sentinel in the disk cache + the
  existing PE-side "null = failed, cached" entry), so a broken shader doesn't
  re-spawn `xcrun` every frame.
- **Degradation is reported, never branched on.** `ret_status` surfaces
  HIT/COMPILED/FELL_BACK/FAILED for telemetry; the PsoWorkers path treats
  anything with a non-zero `ret_library` identically. Because the whole compile
  already runs on the **low-priority PsoWorkers pool** (existing), even a
  fallback live-compile lands off the frame thread — the async architecture and
  the fallback architecture reinforce each other.

---

## 6. Future-proofing — what changes on a backend swap

CLI → in-proc MSC/airconv → Metal 4: the blast radius is *one file*.

- **Add a `CompileBackend` subclass in Layer C, register it ahead of `Cli`.**
  Done. No ABI change (§3 verb is backend-blind). No PE change. No winemetal
  change.
- **If the new backend wants an earlier input** (MSC wants DXIL-ish, airconv
  wants DXBC), it advertises `accepts()`; Layer A consults a tiny capability
  query at init and, *only then*, hands the earlier artifact via `source_kind`.
  spirv-cross becomes skippable, not rewired.
- **Metal 4's `MTL4Compiler`** slots in as either a new library backend *or* a
  replacement pipeline-cache module (§4) — and because library and pipeline are
  separate (§4), adopting it for one doesn't disturb the other.
- **Driver-agnostic by construction:** nothing in any interface mentions GTA IV,
  D3D9, or DXVK. Inputs are `(blob, kind, target)`; keys are content hashes;
  outputs are `.metallib`/PSO handles. A future d8/d11 front-end reuses Layer C
  verbatim. The only D3D-aware code (reflection, PsoKey packing) is already
  quarantined in Layer A where it belongs.

---

## North star

A shader is identified by a content hash, compiled exactly once in its lifetime
by whichever backend happens to be best and present, its `.metallib` persisted
native-side and reloaded with `newLibraryWithData` forever after — while the x86
front-end, blind to whether that backend is a subprocess, an in-process LLVM, or
Apple's converter, simply asks "give me a library for this key" across a single
unchanging verb and gets a handle back; bytes never leave the native side, the
winemetal drop is never forked, specialization and PSO caching sit as clean
independent layers above and below the library cache, and the live source
compiler survives only as the invisible floor that guarantees we are never worse
than today and that swapping the whole compile engine is a one-file change.
