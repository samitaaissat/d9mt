# d9mt — Compiled-Shader Disk Cache: Persistence & Data Architecture

Status: proposal. Scope: the persistence/data layer only. Goal: compile each
MSL→metallib **once ever**, persist the metallib (AIR) blob, and on every later
launch load it with `newLibraryWithData` so the stuttering source compiler
(`newLibraryWithSource`, holds Metal's internal compile lock — `unix.m:33`) never
runs for a shader we have already seen.

This design is opinionated. Where it diverges from the DXMT reference
(`/tmp/dxmt-study`), the reasoning is called out.

---

## 0. What we are caching, and where the stutter actually is

Two compile levels exist (confirmed in code):

| Level | Code site | Cost | Cacheable artifact |
|---|---|---|---|
| **L1: MSL → metallib (AIR)** | `unix.m` `newLibraryWithSource` via `D9MT_FUNC_NEW_LIBRARY_FROM_SOURCE` (`d9mt_shader.cpp:292`) | **the big stutter** (~90 ms, internal Metal lock) | `.metallib` blob (MTLB container of AIR bitcode) |
| **L2: metallib → GPU ISA** | `unix.m` `newRenderPipelineStateWithDescriptor` via `D9MT_FUNC_NEW_RENDER_PSO` (`d9mt_shader.cpp:897`) | lighter, but non-trivial in bursts | `MTLBinaryArchive` blob |

There is also a **function-specialization** step *between* them:
`newFunctionWithConstants` (`d9mt_shader.cpp:418`) bakes spec-constant values into
an `MTLFunction` drawn from the L1 library. **This is in-memory only and is NOT a
persistence boundary** — it is cheap, and the metallib already contains the
unspecialized function plus its `function_constants`. We cache L1 and L2; we do
**not** cache specialized functions. (Justification in §5.)

**Decision: persist BOTH L1 and L2, in one store, as two record namespaces.**
L1 is mandatory (kills the stutter). L2 is a strict, independent win on top.
They are keyed differently and one does not imply the other.

---

## 1. Cache Key — what identifies an artifact

### Principle

A cache key must be a **pure function of every input that can change the bytes of
the output artifact**, and *nothing else*. If two inputs that would produce
different AIR collide on one key, we serve a wrong shader → GPU corruption or
crash. If one input that does *not* affect output is included, we get spurious
misses (re-compile, re-stutter) — merely a perf bug. So: **err toward over-keying
for correctness, but identify the true inputs precisely.**

### 1a. L1 key (MSL → metallib)

The output of L1 is fully determined by the **exact MSL source bytes** plus the
**compiler options** plus the **toolchain**. Therefore:

```
L1_key = H( domain_tag="L1"
          ‖ codegen_epoch        (u32, see §2)
          ‖ msl_source_bytes     (the exact std::string we feed newLibraryWithSource)
          ‖ msl_lang_version     (u16: 3.0 today; from CompilerMSL::Options)
          ‖ fast_math            (u8: lp.fast_math)
          ‖ toolchain_id )       (see §2 — metal compiler / OS build identity)
```

**Why hash the MSL text, not the upstream DXSO/SPIR-V/DxvkShader hash?**
The MSL text is the *last* representation before the artifact, so it is the
**tightest correct key**: it transparently absorbs every upstream transform
(DXVK getCode module fixups, spirv-cross codegen, undefined-input elimination,
RT swizzles, flat-shading — all of `compileShader` in `d9mt_shader.cpp:121`)
without us having to enumerate them. If we keyed on the SPIR-V or the
`DxvkShader*`+`DxvkShaderModuleCreateInfo` instead (what the in-memory
`s_compileCache` does at `d9mt_shader.cpp:317`), then **any** change to
spirv-cross or our reflection codegen could silently produce different MSL under
the *same* key → stale-cache corruption. Keying on the final MSL makes the codegen
version *almost* redundant for correctness — but we still fold in `codegen_epoch`
as a cheap belt-and-suspenders global kill switch (§2).

DXMT keys on the SHA-1 of the **DXBC** plus a variant struct (`d3d11_pipeline_cache.cpp:73`),
because its airconv compiler is in-tree and versioned by `kDXMTShaderCacheVersion`.
We deliberately key one layer lower (final MSL) because our codegen path is a
**stack of third-party libraries** (DXVK + spirv-cross vendored under
`vendor/`) whose output we do not fully control — the MSL bytes are the only
thing we can be *sure* matches the artifact.

Note: the `DxvkShaderModuleCreateInfo` permutations (`fsDualSrcBlend`,
`fsFlatShading`, `undefinedInputs`, `rtSwizzles` — `d9mt_context.cpp:754`)
produce **different MSL text**, so they are *automatically* distinct L1 keys.
No special handling needed; this is a direct benefit of MSL-text keying.

Spec constants are **NOT** in the L1 key — they are function-constants baked at
L2/specialization time and live unspecialized inside the one metallib. One
metallib serves all spec variants of a shader. (§5.)

### 1b. L2 key (metallib → PSO / MTLBinaryArchive)

L2's output depends on the two specialized functions *and* the full pipeline
state. The functions are derived from L1 metallibs plus spec values:

```
L2_key = H( domain_tag="L2"
          ‖ codegen_epoch ‖ toolchain_id ‖ gpu_id   (§1c)
          ‖ L1_key(vs)    ‖ vs_spec_values[]         (resolved per getShaderFunction)
          ‖ L1_key(fs)    ‖ fs_spec_values[]
          ‖ pso_state_blob )                          (the packed d9mt_pso_info, §5)
```

This reuses the existing in-memory `PsoKey` semantics (`d9mt_context.cpp:710`:
vs/fs pointers + packed `DxvkGraphicsPipelineStateInfo`) but replaces the
**pointer identity** of vs/fs with their **content identity** (their L1 keys), so
the key is stable across process launches. The 6 spec dwords already live in
`key.state.sc.specConstants` (`d9mt_context.cpp:769`) and are folded in here.

### 1c. Device / GPU identity

AIR (L1) is **GPU-family-portable** within a Metal version, but compiled PSO ISA
(L2) is **not** — it targets a specific GPU. So:

- `gpu_id` (registryID / GPU name + family) → **L2 key only**.
- L1 keys omit `gpu_id`: one AIR blob is valid for any Apple-silicon GPU on the
  same toolchain. This lets the L1 cache stay portable (and, in principle,
  shippable/seedable — see §7).

### 1d. Hash function

**Use a 128-bit (or 256-bit) cryptographic-strength-enough digest, not FNV.**

- The existing in-memory hashers (`PsoKeyHash` 64-bit FNV, `CompileKeyHash`)
  are fine for a hash *bucket* but are **unacceptable as a disk primary key**:
  a 64-bit FNV over thousands of shaders has a non-negligible birthday-collision
  probability, and a collision here is a *silent wrong-shader load*.
- Recommendation: **BLAKE3** (fast, 256-bit, no dependency baggage) truncated to
  128 bits for the key, *or* SHA-256 if we want zero new deps (DXMT ships SHA-1;
  SHA-1 is adequate for non-adversarial collision resistance but I'd pick a
  256-bit function for headroom and to avoid the SHA-1 stigma). The MSL strings
  are small (hundreds of bytes to a few KB), so hashing cost is irrelevant next
  to a 90 ms compile.
- **Collision strategy:** store the **full key bytes** as the DB primary key
  (not a truncated hash of a hash). On `get`, after a key match we **also verify
  the stored artifact's self-consistency** (§6 — MTLB magic + length + CRC) and,
  for L1, optionally store the source-length alongside so a 1-in-2^128 collision
  still cannot feed a length-mismatched blob to `newLibraryWithData`. In practice
  128-bit collisions never happen; the value-side integrity check is the real
  safety net.

---

## 2. Versioning & Invalidation

Three independent things can invalidate the cache. **Conflating them into one
integer (as DXMT's single `kDXMTShaderCacheVersion` does) is too coarse** — it
forces a *total* wipe when only one axis moved. We separate them:

1. **`codegen_epoch` (u32, compile-time constant in d9mt).**
   Bumped by *us* whenever our translation pipeline changes in a way that alters
   MSL bytes for the same input: spirv-cross vendored-version bump, reflection
   changes, new module fixups, MSL option changes. This is the manual "I changed
   the codegen, distrust everything" lever. Folded into **both** L1 and L2 keys.
   Cheap insurance even though MSL-text keying already catches most cases.

2. **`toolchain_id` (derived at runtime, not a constant).**
   Identifies the Metal *compiler* that produced the AIR. Best source: the
   `MTLLanguageVersion` plus the **OS/Metal framework build** (e.g. the Metal
   framework bundle version, or `kCFCoreFoundationVersionNumber`, or the result
   of a one-time probe-compile fingerprint). When macOS updates Metal, AIR ABI
   *can* shift; `newLibraryWithData` will then reject the blob — but we want to
   **invalidate proactively** rather than rely on per-load rejection. Folded into
   both keys.

3. **`gpu_id`** — L2 only (§1c).

### How invalidation actually happens (the elegant part)

Because all three version axes are **inside the key**, invalidation is
**automatic and lazy**: a changed axis simply produces *different keys*, so old
entries become unreachable and new entries are written alongside. **No explicit
"wipe on version mismatch" code path is needed for correctness.** This is
strictly better than DXMT's table-name-per-version scheme (`cache_%llu`,
`cache.c:51,131`), which orphans an entire prior table on every bump.

### But: garbage must not accumulate forever

Unreachable entries (old epochs/toolchains) are dead weight. Policy:

- **Single physical table** with explicit columns
  `(key BLOB PK, value BLOB, codegen_epoch INT, toolchain_id BLOB, last_used INT, size INT)`.
  Storing the version axes as **columns** (not baked only into an opaque key)
  lets us run cheap **GC sweeps**: on startup, asynchronously
  `DELETE FROM cache WHERE codegen_epoch < :current_epoch` and prune by
  `last_used` LRU once total size exceeds a cap (e.g. 512 MB — GTA IV's full set
  is far smaller, low hundreds of MB at most). This is the migration/eviction
  policy: **never auto-wipe wholesale; GC selectively and lazily.**
- `last_used` is updated opportunistically (batched, not per-get, to avoid write
  amplification) so LRU is meaningful.
- A `DXMT_SHADER_CACHE=0`-style env (`D9MT_SHADER_CACHE=0`) disables the whole
  layer; `D9MT_SHADER_CACHE_WIPE=1` force-drops the table on open for support.

**Should it ever be wiped vs grow forever?** Grow, but **bounded by an LRU cap**
and **swept by epoch**. Never unbounded. Never auto-wiped on version change
(keys handle that). Manual wipe only as an escape hatch.

---

## 3. Storage Format & Location

### The big decision: one keyed DB vs many `.metallib` files vs packed archive

**Choose: one embedded key/value DB (SQLite), reusing winemetal's existing
`CacheReader`/`CacheWriter` thunks** (`cache.c`), generalized for our two
namespaces. Rationale for the GTA IV workload (thousands of small blobs):

- **Many per-shader files** (`xxx.metallib` on disk): rejected. Thousands of
  tiny files means thousands of `open`/`stat`/`read` syscalls *through the
  Wine→Rosetta→host FS path*, terrible directory-enumeration cost, and a
  fragile atomic-write story (temp-file + rename per shader). Inode pressure.
  The only thing it buys is `newLibraryWithData` being able to mmap a file — not
  worth it.
- **One packed/append-only archive** (custom format): rejected as
  reinvention. We'd have to build our own index, free-list, compaction, crash
  recovery, and concurrent-writer protocol — i.e. re-implement a database badly.
- **SQLite KV DB**: chosen. It already exists in the codebase
  (`cache.c` + `sqlite3`), gives us **atomic transactions, WAL concurrency,
  crash recovery, a B-tree index, and single-file management** for free. Blobs
  of ~10–200 KB are exactly SQLite's sweet spot. The whole GTA IV shader set is
  one file we can ship/copy/delete trivially.

This is the **same engine** DXMT uses; we **extend** rather than fork it.

### Schema (one file, generalized from DXMT's `cache_<ver>` table)

A single SQLite file with **two logical namespaces** distinguished by a
`domain` column (or a 1-byte domain prefix inside the key — column is clearer):

```sql
CREATE TABLE IF NOT EXISTS shaders (
  key            BLOB PRIMARY KEY,   -- full 128/256-bit digest (§1)
  domain         INTEGER NOT NULL,   -- 1 = L1 metallib, 2 = L2 PSO archive
  value          BLOB NOT NULL,      -- metallib bytes (L1) / MTLBinaryArchive (L2)
  source_len     INTEGER,            -- L1: original MSL length (collision belt)
  codegen_epoch  INTEGER NOT NULL,
  toolchain_id   BLOB NOT NULL,
  gpu_id         BLOB,               -- L2 only
  crc32          INTEGER NOT NULL,   -- of value (§6)
  size           INTEGER NOT NULL,
  last_used      INTEGER NOT NULL
) WITHOUT ROWID;
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
```

`WITHOUT ROWID` because the key *is* the identity (clustered B-tree on the
digest). DXMT's plain `key BLOB PRIMARY KEY` table is the same idea; we add the
metadata columns that enable §2's selective GC and §6's integrity.

### Location

Follow DXMT exactly — it is correct:

- Default under **`_CS_DARWIN_USER_CACHE_DIR`** (`cache.c:26`), i.e. the
  Wine-prefix/sandbox-mapped equivalent of `~/Library/Caches/...`. This is the
  *right* OS-blessed location for regenerable derived data: it survives reboots,
  is excluded from Time Machine, and is auto-purged by macOS under disk
  pressure (a free safety valve for "disk full").
- **Per-exe** subdir: `d9mt/<exeName>/shaders.db` (DXMT:
  `dxmt/<exe>/shaders_<ver>.db`, `dxmt_shader_cache.cpp:29`). Per-exe because
  GTA IV's shader set is disjoint from other titles and per-exe keeps each DB
  small and its LRU/GC scoped. We drop the `_<ver>` filename suffix because our
  version axes live *in keys/columns* now (§2), not in the filename.
- Override env `D9MT_SHADER_CACHE_PATH` (absolute path), mirroring
  `DXMT_SHADER_CACHE_PATH`.

**Why not global/shared across games?** A shared L1 store *could* be valid
(AIR is GPU-portable, and identical engine shaders across titles would dedupe),
but per-exe is simpler, keeps concurrency contained, and the dedup upside for a
single-title use case (GTA IV) is nil. Keep per-exe; revisit a shared L1 pool
only if we ever ship many titles.

---

## 4. Concurrency & Multi-Process

Two concurrency dimensions: (a) our own **async PSO worker threads**
(`PsoWorkers`, up to 4, `d9mt_context.cpp:977`) all compiling and wanting to
write; (b) **multiple processes** (two games, or two instances) sharing a DB
file — and macOS may also itself purge the Caches dir mid-run.

### In-process

- **One process-global `CacheWriter` + one `CacheReader`**, each behind its own
  mutex — exactly DXMT's `LockProtected<>` model
  (`dxmt_shader_cache.hpp:43`). The worker threads already do the heavy compile
  *outside* any cache lock; they take the writer mutex only for the
  millisecond-scale `set`. With SQLite `NOMUTEX` + our own mutex this is
  correct and contention is negligible (a 90 ms compile dwarfs a sub-ms insert).
- **Read-before-compile in the worker.** `compilePso`/`getCompiledShader` must
  first try `reader.get(L1_key)` → if hit, `newLibraryWithData` (fast, no
  source compiler). Only on miss do we run `newLibraryWithSource` and then
  `writer.set(L1_key, metallibBytes)`. Same pattern for L2.
- **Duplicate-work tolerance.** Two workers may miss and compile the *same*
  shader concurrently (e.g. two PSOs sharing a VS). That is acceptable: both
  produce identical AIR, both `set` the same key, last write wins (`INSERT OR
  REPLACE`). We do **not** add a cross-worker "compile in flight" registry for
  the disk layer — the existing in-memory `s_compileCache`
  (`d9mt_shader.cpp:340`) already dedupes within a process, so cross-worker
  disk dupes are rare and harmless.

### Multi-process / atomicity / torn writes

- **SQLite WAL gives us atomic, all-or-nothing commits** — there is no torn
  *logical* write. A `set` either fully lands or not at all; a crash mid-write
  leaves the DB at the last committed state (WAL replay on next open). This is
  the single biggest reason to use SQLite over hand-rolled files.
- **The DB *open* is the only true cross-process race** (table create). DXMT
  guards exactly this with an **`flock(LOCK_EX)` on a `<db>-lock` sidecar file**
  during open/`CREATE TABLE` (`cache.c:110-143`). We keep that. After open, WAL
  handles concurrent readers + one writer per process safely; multiple *writer
  processes* serialize via SQLite's own locking (brief, since writes are tiny).
- **`synchronous=NORMAL` + WAL** (DXMT's choice) is the right durability/perf
  point: we can lose the *last few* unflushed inserts on a hard power cut, but
  never corrupt the file, and we never block the compile on fsync. Losing a few
  cached blobs just means re-compiling them next launch — fully degradable.

---

## 5. Coverage — L1, L2, and spec variants, and how they relate in storage

### L1 (metallib) — primary, mandatory

One row per `(MSL-text, options, toolchain)`. This is the stutter-killer and the
backbone of the cache. On hit: `newLibraryWithData` instead of
`newLibraryWithSource`. **A new unixlib entry point is required** —
`D9MT_FUNC_NEW_LIBRARY_FROM_DATA` wrapping `newLibraryWithData:` (the proven
path) — plus a `D9MT_FUNC_*` for serializing a freshly source-compiled library
back to bytes if we choose to compile-then-store rather than store the
out-of-process `xcrun metal` output. (Two viable producers: in-process
`newLibraryWithSource` then `MTLLibrary`→data, or the proven out-of-process
`xcrun metal -o x.metallib`. Either yields the blob we persist; persistence
layer is producer-agnostic.)

### L2 (PSO ISA via MTLBinaryArchive) — secondary, independent win

Metal's `MTLBinaryArchive` serializes compiled pipeline state to a blob we can
persist and later feed back as a descriptor's `binaryArchives` to make
`newRenderPipelineState` a cache *lookup* instead of a *compile*. Store one row
per L2 key (§1b). **This requires new unixlib entries** to (a) create/loadphase
an `MTLBinaryArchive` from our stored bytes and attach it to the PSO descriptor,
and (b) `serializeToURL`/extract its bytes after adding a pipeline. Until those
exist, L2 is a no-op stub — **L1 alone removes the dominant stutter**, so L2 is a
clean phase-2.

**Relationship in storage:** L1 and L2 are **separate rows, separate domains,
no foreign-key coupling.** An L2 entry is *logically* derived from two L1 entries
(its key embeds their L1 keys, §1b), but we do **not** enforce referential
integrity in the DB. If an L2 row exists but its L1 metallibs were GC'd, the L2
load simply misses/fails its integrity check and we fall back to live compile.
Decoupling keeps GC simple and avoids cascade-delete logic.

### Spec-constant variants — deliberately NOT persisted

`getShaderFunction` (`d9mt_shader.cpp:369`) specializes via
`newFunctionWithConstants`. **We do not give spec variants their own L1 rows.**
Reasons:

- The **unspecialized** metallib already encodes the function + its
  `function_constants`; specialization is **cheap and local** (no source
  compiler, no Metal compile lock) — it is not a stutter source.
- Persisting per-variant `MTLFunction`s is impossible anyway (`MTLFunction` has
  no stable serialized form independent of its library).
- The spec values **do** matter for **L2** (they change the final ISA), and they
  are already folded into the L2 key (§1b). That is the correct and *only* place
  spec variants enter persistence.

So: **L1 = per-shader-MSL (spec-agnostic). L2 = per-(shader-pair × pso-state ×
spec-values × gpu).** This matches the engine's actual cost structure.

---

## 6. Integrity & Failure Modes — never break the driver

**Invariant: every failure degrades to live `newLibraryWithSource` compile.**
The cache is *only* an optimization; a draw must never fail because of it. Each
failure mode and its handling:

| Failure | Detection | Handling |
|---|---|---|
| **Corrupt / truncated value** | `crc32` column mismatch vs `value`; for L1 also `length`/MTLB-magic (`MTLB` header) sanity-check before `newLibraryWithData` | Treat as miss → live compile → overwrite the bad row (`INSERT OR REPLACE`). Log once. |
| **`newLibraryWithData` rejects blob** (toolchain ABI drift not caught by `toolchain_id`) | Non-zero NSError / null library from the unixcall | Fall through to `newLibraryWithSource`; delete the offending key so we don't retry it; the fresh compile re-`set`s a valid blob. |
| **Partial write / crash mid-set** | Impossible to observe a partial logical row — **WAL atomicity** (§4) | Nothing to do; DB is always at a committed boundary. |
| **DB file itself corrupt** (`SQLITE_CORRUPT` on open/read) | SQLite return codes | Catch at open: **rename the corrupt file aside (`.db.corrupt`) and recreate empty**, or just disable the cache for the session and log. Driver runs fully on live compile. |
| **`xcrun metal` / toolchain absent** (if using out-of-process L1 producer) | producer returns error / non-zero exit | Fall back to in-process `newLibraryWithSource` (always available). Optionally disable disk persistence for the session. The driver never depends on the toolchain being present. |
| **Disk full / write fails** | `set` returns `SQLITE_FULL`/IO error | Swallow it (log once), keep running. The Caches location is also auto-purgeable by macOS, mitigating this. We never block or crash on a failed write. |
| **`open`/lock contention** | `flock` (§4) | Standard blocking flock during the brief open window; readers never block writers post-open (WAL). |
| **Collision (1-in-2^128)** | Effectively impossible; if it somehow matched, the **value-side CRC + L1 length/magic** check still guards the actual `newLibraryWithData` | Treated as corrupt → live compile. |

Key design rule: **reads are best-effort and side-effect-light; the only hard
requirement is that a poisoned entry is detected and bypassed, then healed by the
next live compile.** Because the integrity check is on the *value* (CRC + format
magic), not just the key, even a hash collision or a foreign/garbage blob cannot
reach `newLibraryWithData` with wrong-but-loadable bytes.

---

## 7. Things I'd reject (and why)

- **64-bit FNV as a disk key** (reusing `PsoKeyHash`): rejected — collision risk
  on a persistent multi-thousand-entry store is a *correctness* bug, not a perf
  bug. 128-bit minimum. (In-memory FNV is fine; it's just a bucket index.)
- **DXMT's single integer cache-version as the sole invalidation lever**
  (`kDXMTShaderCacheVersion`, table-name-per-version): rejected as too coarse —
  forces total orphaning on any one-axis change. We split into
  `codegen_epoch` / `toolchain_id` / `gpu_id` folded into keys, with column-based
  selective GC.
- **Per-shader `.metallib` files**: rejected — syscall + inode + atomic-write
  cost across the Wine/Rosetta FS boundary; reinvents indexing.
- **Custom packed archive**: rejected — reinvents a database; crash-recovery and
  concurrent-write correctness are hard and SQLite already solves them.
- **Persisting specialized `MTLFunction`s / per-spec-variant L1 rows**: rejected
  — specialization is cheap and not the stutter; spec values belong in the L2 key.
- **Referential integrity / cascade delete between L2 and L1**: rejected —
  decoupled rows + integrity-check-on-load is simpler and equally safe.
- **A "compile in flight" cross-worker registry for the disk layer**: rejected —
  in-memory `s_compileCache` already dedupes per process; cross-worker dupes are
  rare and idempotent (`INSERT OR REPLACE`).
- **Shipping/seeding a precompiled cache** (future, not now): the L1 store is
  GPU-portable so it *could* be shipped as a warm cache — but only if
  `toolchain_id` matches the user's Metal build, which we can't guarantee. Park
  it; the keying already makes it safe if we ever want it.

---

## 8. Data-Model Summary (implementable spec)

**Store:** one SQLite file per exe at
`$_CS_DARWIN_USER_CACHE_DIR/d9mt/<exeName>/shaders.db` (override:
`D9MT_SHADER_CACHE_PATH`; disable: `D9MT_SHADER_CACHE=0`). WAL,
`synchronous=NORMAL`, `flock` sidecar guarding open/create. Reuse + generalize
winemetal's `CacheReader`/`CacheWriter` thunks (`cache.c`).

**Table** `shaders WITHOUT ROWID`:
`key BLOB PK, domain INT, value BLOB, source_len INT, codegen_epoch INT,
toolchain_id BLOB, gpu_id BLOB, crc32 INT, size INT, last_used INT`.

**Domains:** `1 = L1` (MSL→metallib blob), `2 = L2` (MTLBinaryArchive PSO blob).

**Keys** (digest = BLAKE3-128 or SHA-256; full digest stored as PK):
- `L1 = H("L1" ‖ codegen_epoch ‖ msl_bytes ‖ msl_lang_version ‖ fast_math ‖ toolchain_id)`
- `L2 = H("L2" ‖ codegen_epoch ‖ toolchain_id ‖ gpu_id ‖ L1key(vs) ‖ vs_spec[] ‖ L1key(fs) ‖ fs_spec[] ‖ pso_state_blob)`

**Version axes** (all in-key + as columns for GC):
`codegen_epoch` (our compile-time const), `toolchain_id` (Metal/OS build
fingerprint, runtime), `gpu_id` (L2 only, runtime).

**Read path (per shader, in worker):**
`reader.get(L1key)` → verify `crc32` + MTLB magic + `source_len` →
`newLibraryWithData` (NEW unixlib entry). Miss/fail →
`newLibraryWithSource` (existing) → `writer.set(L1key, blob, meta)`.
L2 analogous via `MTLBinaryArchive` (phase 2; new unixlib entries).

**Concurrency:** process-global reader+writer, each mutex-guarded; heavy compile
runs outside locks; WAL serializes cross-process writers; duplicate compiles
tolerated (idempotent `INSERT OR REPLACE`).

**Invalidation/GC:** automatic via keys; startup async sweep
`DELETE WHERE codegen_epoch < current` + LRU prune over a size cap
(~512 MB). Never auto-wipe wholesale; manual `D9MT_SHADER_CACHE_WIPE=1`.

**Failure rule:** any miss/corruption/toolchain-absence/disk-full → silently
degrade to live `newLibraryWithSource`; heal the row on next compile. The cache
can never break a draw.

**New unixlib surface required** (`d9mtmetal.h` / `unix.m`):
`D9MT_FUNC_NEW_LIBRARY_FROM_DATA` (mandatory, phase 1), library→bytes serializer
if storing in-process output, and `MTLBinaryArchive` create/attach/serialize
entries (phase 2, L2). Cache reader/writer thunks already exist in winemetal.
