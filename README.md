# d9mt — D3D9 on Metal for Apple Silicon

A from-scratch **Direct3D 9 → Metal** driver for Wine/CrossOver on Apple
Silicon. **No Vulkan, no MoltenVK** — D3D9 calls are translated to Metal
directly.

It pairs DXVK's battle-tested D3D9 front-end (D3D9 → SPIR-V → MSL) with a
custom Metal backend that talks to the GPU through DXMT's `winemetal` bridge.

> **Tested with exactly one game so far.** It has only been run against a
> single D3D9 title (GTA IV) — no other DirectX 9 game has been tried yet, so
> expect unimplemented paths on anything else.

```
YourGame.exe  (32-bit Windows PE, runs under Rosetta 2)
   │  loads
   ▼
d3d9.dll      ← THIS PROJECT (mingw PE):  DXVK D3D9 front-end + d9mt Metal backend
   │            DXSO bytecode → SPIR-V (DXVK) → MSL (spirv-cross) → metallib
   ▼
winemetal.dll / .so   (DXMT prebuilt, Wine builtin)  ──►  Metal  ──►  CAMetalLayer
   ▲
d9mtmetal.so  (our native arm64 Wine unixlib: PSO creation, MSL→metallib compile, shader disk cache)
```

## Status

- **Playable.** A real, draw-call-heavy D3D9 title renders and runs end-to-end
  on an M1 Max at roughly 50-90 fps depending on how many draws a scene issues.
- **Shader disk cache**: each shader compiles once *ever* (out-of-process via
  the Metal toolchain), is cached to disk, and reloads instantly — no compile
  stutter, fast warm boot.
- Performance work landed: async pipeline compilation, command batching,
  buffer suballocation, residency dedup. Frame pacing is clean.
- This is research-grade software for a single platform (Apple Silicon +
  CrossOver). Expect rough edges on untested games.

## Requirements

- **Apple Silicon** Mac (M1/M2/M3…), macOS 14+.
- **CrossOver** (tested on CrossOver 26, which bundles DXMT/winemetal). A
  plain Wine prefix with the DXMT winemetal builtin also works.
- **Xcode Command Line Tools** + the **Metal toolchain** — `xcrun metal` must
  resolve. The native unixlib links `Metal`/`Foundation`, and the shader cache
  shells out to `metal` to compile MSL → metallib. Check:
  `xcrun --sdk macosx --find metal`.
- **mingw-w64** (both `i686-w64-mingw32` and `x86_64-w64-mingw32`) — e.g.
  `brew install mingw-w64`.
- **glslang** (`brew install glslang`), **python3**, and a working `sqlite3`
  (system-provided on macOS).

## Build

Three steps, from the repo root. DXVK and spirv-cross are vendored in
`vendor/` — nothing to fetch for the front-end.

```bash
# 1. Pull DXMT's winemetal binaries into prebuilt/ (needed to link against).
bash scripts/fetch-winemetal.sh

# 2. Build the native companion unixlib (d9mtmetal.so + PE shims) and install
#    it into CrossOver. Set BOTTLE to your game's bottle name.
BOTTLE="My Game" bash tools/build-d9mtmetal.sh

# 3. Build the driver itself -> build/d3d9fe.dll
bash scripts/build-dxvkfe.sh
```

### Release (max-speed) build

For a production build — `-O3`, asserts stripped, and the in-engine profiler +
file logging **compiled out** (zero hot-path instrumentation):

```bash
RELEASE=1 bash scripts/build-dxvkfe.sh
```

It uses a separate object cache (`build/dxvkfe-obj-release`), so it won't clash
with the default dev build.

## Install & run (any bottle, any game)

1. **Deploy the driver as `d3d9.dll`** into your game's folder:

   ```bash
   GAME_DIR="$HOME/Library/Application Support/CrossOver/Bottles/My Game/drive_c/.../YourGame"
   cp build/d3d9fe.dll "$GAME_DIR/d3d9.dll"
   ```

2. **Launch** with `d3d9` overridden to the native (our) DLL:

   ```bash
   CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
   WINEDLLOVERRIDES="d3d9=n" "$CX/bin/wine" --bottle "My Game" \
     start /unix "$GAME_DIR/YourGame.exe"
   ```

Step 2 of the build already installed `winemetal`/`d9mtmetal` into the bottle
and registered them as Wine builtins. If a game ships its own `d3d9.dll`, your
copy in the game folder takes precedence with the override above.

## Tuning (environment variables)

All performance features are **on by default**; set to `0` to disable for A/B.

| var | default | effect |
|-----|---------|--------|
| `D9MT_METALLIB_CACHE` | on | shader disk cache (compile once, reload forever) |
| `D9MT_ASYNC` | on | compile pipelines on background threads (no compile-freeze) |
| `D9MT_BATCH` | on | batch render commands into one bridge crossing |
| `D9MT_SUBALLOC` | on | suballocate dynamic buffers (kills DISCARD churn) |
| `D9MT_SHADER_CACHE_PATH` | — | override cache location (default `~/Library/Caches/d9mt/<exe>/`) |
| `D9MT_TRACE` | off | `=1` writes a per-frame CPU breakdown to `d3d9fe-trace.log` (dev builds only; adds overhead) |
| `D9MT_DUMP_MSL` | off | dump generated MSL to a Windows path, e.g. `C:\msl` |

Example — run with the cache off to measure cold compile:
`D9MT_METALLIB_CACHE=0 wine ...`

- SIP can stay enabled; the driver itself doesn't require disabling it.

## How it works (deeper)

- The DXVK D3D9 front-end (vendored) turns the game's DXSO shader bytecode into
  SPIR-V and tracks all D3D9 state; spirv-cross turns SPIR-V into MSL.
- The d9mt backend (`src/d3d9fe/`) replaces DXVK's Vulkan layer: it builds
  Metal render pipelines, argument buffers, and command streams, and submits
  them through `winemetal`.
- `d9mtmetal` (`src/d9mtmetal/`) is our small native Wine unixlib for the two
  things `winemetal` doesn't expose: pipeline-state creation with a vertex
  descriptor, and the MSL→metallib compile + on-disk shader cache.
- See `docs/` for the architecture notes (Metal backend, shader cache design).

## Credits

- **[DXVK](https://github.com/doitsujin/dxvk)** — the D3D9 front-end (DXSO →
  SPIR-V) and spirv-cross plumbing, vendored under `vendor/`.
- **[DXMT](https://github.com/3Shain/dxmt)** — the `winemetal` bridge that
  carries Metal calls across the Wine wow64 boundary.
- **spirv-cross** — SPIR-V → MSL.

## License

See the licenses of the vendored components (DXVK, spirv-cross) under
`vendor/`. d9mt's own code follows suit.
