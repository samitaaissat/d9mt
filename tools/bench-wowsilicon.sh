#!/usr/bin/env bash
# bench-wowsilicon.sh — run d9mt test/bench exes under a COPY of the
# WoWSilicon bundled wine runtime. Never mutates the app bundle or the
# live runtime: the runtime tree is copied into the env dir and the fresh
# d3d9.dll + d9mtmetal pair replace the copies inside that copy (the
# pairing rule: PE dll and .so must come from the same build).
#
# Usage:
#   tools/bench-wowsilicon.sh install            # stage build/ artifacts into the env
#   tools/bench-wowsilicon.sh run <exe> [K=V...] # run one exe under the env, tee output
#   tools/bench-wowsilicon.sh reset-runtime      # re-copy the runtime from D9MT_RT_SRC
#
# Env knobs:
#   D9MT_BENCH_ENV    scratch env dir   (default: WoWSilicon/.d9mt-work/bench-env)
#   D9MT_RT_SRC       source "WoWSilicon Game.app" to copy (default: the staged
#                     runtime inside .build/WoWSilicon.app — discovered pass-3 Task 0)
#   D9MT_RUN_TIMEOUT  per-run wall-clock limit in seconds (default 900)
#
# Launch adaptations vs the CrossOver-bottle flow (from WoWSilicon's
# LaunchService/PatchService — see pass-3 task-0 report):
#   - exe is invoked by its Unix path with CWD=$GAME (wine maps it via Z:);
#     result files (*_out.txt) therefore land in $GAME.
#   - WINESERVER is pinned to the runtime copy (wine cannot derive
#     <bin>/wineserver under the nested game .app layout).
#   - X87_SIDECAR_PATH points at wine-rosetta-shim: wine (runtime patch 0002)
#     re-execs i386 images as [$X87_SIDECAR_PATH, --cooperative, loader, ...],
#     and the shim rewrites the loader arg to wine-gamemode and execs
#     x87sidecar beside it (Game Mode loader geometry preserved).
#     NOTE: the legacy ROSETTA_X87_PATH var is what pass-3 used; runtime-v4
#     replaced rosettax87 with athei/x87sidecar, and setting the OLD var makes
#     the shim try to execv a rosettax87 that no longer ships ("execv
#     rosettax87: No such file or directory").
#   - winemetal/d9mtmetal must resolve as WINE BUILTINS for the native
#     d3d9.dll's imports: the PE stubs are staged into the prefix
#     (system32/syswow64/x86_64-unix) and registered builtin via
#     HKCU\Software\Wine\DllOverrides; find_builtin_dll then loads the real
#     pair from the runtime copy's lib/wine arch dirs.
#   - No `arch -x86_64`: Contents/MacOS/wine is an x86_64-only Mach-O, the
#     kernel routes it through Rosetta automatically.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENV_DIR="${D9MT_BENCH_ENV:-/Users/sami.taaissat/Documents/Perso/WoWSilicon/.d9mt-work/bench-env}"
RT_SRC="${D9MT_RT_SRC:-/Users/sami.taaissat/Documents/Perso/WoWSilicon/.build/WoWSilicon.app/Contents/SharedSupport/WoWSilicon Game.app}"
GAME="$ENV_DIR/game"          # bench exes + d3d9.dll live here
RT="$ENV_DIR/WoWSilicon Game.app"
PFX="$ENV_DIR/prefix"
LOGS="$ENV_DIR/logs"
WINE="$RT/Contents/MacOS/wine"
WINESERVER="$RT/Contents/MacOS/wineserver"
TIMEOUT="${D9MT_RUN_TIMEOUT:-900}"

die() { echo "[bench-wowsilicon] ERROR: $*" >&2; exit 1; }

need_artifacts() {
  local f
  for f in build/d3d9fe.dll build/d9mtmetal/d9mtmetal32.dll \
           build/d9mtmetal/d9mtmetal64.dll build/d9mtmetal/d9mtmetal.so; do
    [ -f "$ROOT/$f" ] || die "missing $ROOT/$f — build first:
  bash scripts/build.sh && RELEASE=1 bash scripts/build-dxvkfe.sh && \\
  BOTTLE=wowsilicon-unused CX=<fake-cx> HOME=<fake-home> bash tools/build-d9mtmetal.sh"
  done
}

# Every wine invocation shares this environment (LaunchService's launch env,
# minus the game-specific bits). Extra K=V pairs go in WINE_ENV_EXTRA and win
# over the defaults (they are applied last).
wine_do() {
  env \
    WINEPREFIX="$PFX" \
    WINESERVER="$WINESERVER" \
    WINEMSYNC=0 \
    __COMPAT_LAYER=RunAsInvoker \
    WINEDLLOVERRIDES="d3d9=n,b;mscoree=d;mshtml=d;winemenubuilder.exe=d" \
    MTL_HUD_ENABLED=0 \
    D9MT_METALLIB_CACHE=1 \
    D9MT_ASYNC=1 \
    X87_SIDECAR_PATH="$RT/Contents/MacOS/wine-rosetta-shim" \
    PATH="$RT/Contents/MacOS:$PATH" \
    ${WINE_ENV_EXTRA[@]+"${WINE_ENV_EXTRA[@]}"} \
    "$WINE" "$@"
}

copy_runtime() {
  [ -d "$RT_SRC" ] || die "D9MT_RT_SRC not found: $RT_SRC"
  [ -x "$RT_SRC/Contents/MacOS/wine" ] || die "no executable wine in $RT_SRC"
  echo "[bench-wowsilicon] copying runtime: $RT_SRC"
  echo "                   -> $RT  (~630 MB, one-time)"
  mkdir -p "$ENV_DIR"
  cp -R "$RT_SRC" "$ENV_DIR/"
  [ -x "$WINE" ] || die "copied runtime has no executable wine at $WINE"
}

# Mirror the builtin-marked dlls from the runtime copy's arch dirs into the
# prefix and register the overrides (PatchService.installD9MTPrefixSupport +
# registerD9MTBuiltins). Runs on every install so a rebuilt d9mtmetal pair
# always reaches the prefix.
stage_prefix_builtins() {
  local w="$PFX/drive_c/windows"
  mkdir -p "$w/system32" "$w/syswow64" "$w/x86_64-unix"
  cp "$RT/Contents/lib/wine/x86_64-windows/d9mtmetal.dll" "$w/system32/d9mtmetal.dll"
  cp "$RT/Contents/lib/wine/i386-windows/d9mtmetal.dll"   "$w/syswow64/d9mtmetal.dll"
  cp "$RT/Contents/lib/wine/x86_64-unix/d9mtmetal.so"     "$w/x86_64-unix/d9mtmetal.so"
  cp "$RT/Contents/lib/wine/x86_64-windows/winemetal.dll" "$w/system32/winemetal.dll"
  cp "$RT/Contents/lib/wine/i386-windows/winemetal.dll"   "$w/syswow64/winemetal.dll"
  cp "$RT/Contents/lib/wine/x86_64-unix/winemetal.so"     "$w/x86_64-unix/winemetal.so"
  WINE_ENV_EXTRA=(WINEDEBUG=-all)
  wine_do reg add 'HKCU\Software\Wine\DllOverrides' /v winemetal  /d builtin /f >/dev/null
  wine_do reg add 'HKCU\Software\Wine\DllOverrides' /v d9mtmetal  /d builtin /f >/dev/null
}

cmd_install() {
  need_artifacts
  mkdir -p "$GAME" "$LOGS"
  [ -d "$RT" ] || copy_runtime

  echo "[bench-wowsilicon] swapping d9mtmetal pair inside the runtime COPY"
  cp "$ROOT/build/d9mtmetal/d9mtmetal32.dll" "$RT/Contents/lib/wine/i386-windows/d9mtmetal.dll"
  cp "$ROOT/build/d9mtmetal/d9mtmetal64.dll" "$RT/Contents/lib/wine/x86_64-windows/d9mtmetal.dll"
  cp "$ROOT/build/d9mtmetal/d9mtmetal.so"    "$RT/Contents/lib/wine/x86_64-unix/d9mtmetal.so"

  echo "[bench-wowsilicon] staging d3d9.dll + test exes into $GAME"
  cp "$ROOT/build/d3d9fe.dll" "$GAME/d3d9.dll"
  local t
  for t in bench consttest spectest depthbias resettest hdrtext; do
    if [ -f "$ROOT/build/$t.exe" ]; then
      cp "$ROOT/build/$t.exe" "$GAME/"
    else
      echo "[bench-wowsilicon] note: build/$t.exe absent — skipped"
    fi
  done

  if [ ! -d "$PFX/drive_c" ]; then
    echo "[bench-wowsilicon] bootstrapping fresh prefix: $PFX"
    WINE_ENV_EXTRA=(WINEDEBUG=-all)
    wine_do wineboot --init >/dev/null 2>&1 || die "wineboot --init failed"
  fi

  echo "[bench-wowsilicon] staging winemetal/d9mtmetal builtins into the prefix"
  stage_prefix_builtins

  echo "[bench-wowsilicon] install done"
  echo "  runtime : $RT"
  echo "  game dir: $GAME"
  echo "  prefix  : $PFX"
}

cmd_run() { # cmd_run <exe> [K=V ...] [-- exe-args...] — K=V pairs become env
  local exe="$1"; shift
  local kv
  local -a exe_args=()
  WINE_ENV_EXTRA=()
  while [ $# -gt 0 ]; do
    if [ "$1" = "--" ]; then
      shift; exe_args=("$@"); break
    fi
    case "$1" in
      *=*) WINE_ENV_EXTRA+=("$1") ;;
      *) die "run: trailing args must be K=V env pairs (or '--' before exe args), got '$1'" ;;
    esac
    shift
  done
  [ -d "$RT" ] || die "no runtime copy — run '$0 install' first"
  [ -f "$GAME/$exe" ] || die "$GAME/$exe missing — run '$0 install' first"
  mkdir -p "$LOGS"
  local base="${exe%.exe}"
  local logf="$LOGS/$base-$(date +%Y%m%d-%H%M%S).log"
  local outf="$GAME/${base}_out.txt"
  rm -f "$outf"   # never read a stale result from a previous run

  ( cd "$GAME" && wine_do "$GAME/$exe" ${exe_args[@]+"${exe_args[@]}"} ) \
      >"$logf" 2>&1 &
  local pid=$! elapsed=0 rc=0 marker="" marker_at=0
  # Terminal markers in <base>_out.txt: bench prints RESULT..., the tests
  # print PASS/FAIL. resettest deliberately keeps rendering after PASS ("the
  # suite runner kills us"), so once a marker lands we allow a short grace
  # for self-exit, then take the prefix down (wineserver -k kills the
  # subshell's wine children too — kill $pid alone would orphan them).
  while kill -0 "$pid" 2>/dev/null; do
    if [ -z "$marker" ] && [ -f "$outf" ]; then
      marker="$(grep -m1 -E '^(RESULT |PASS|FAIL)' "$outf" 2>/dev/null || true)"
      [ -n "$marker" ] && marker_at=$elapsed
    fi
    if [ -n "$marker" ] && [ $((elapsed - marker_at)) -ge 3 ]; then
      kill "$pid" 2>/dev/null || true
      env WINEPREFIX="$PFX" "$WINESERVER" -k >/dev/null 2>&1 || true
      wait "$pid" 2>/dev/null || true
      case "$marker" in
        FAIL*) rc=1 ;;
        *)     rc=0 ;;
      esac
      break
    fi
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
      echo "[bench-wowsilicon] TIMEOUT after ${TIMEOUT}s — killing wineserver"
      kill "$pid" 2>/dev/null || true
      env WINEPREFIX="$PFX" "$WINESERVER" -k >/dev/null 2>&1 || true
      wait "$pid" 2>/dev/null || true
      rc=124
      break
    fi
    sleep 1; elapsed=$((elapsed + 1))
  done
  if [ -z "$marker" ] && [ "$rc" -eq 0 ]; then
    wait "$pid" || rc=$?
  fi

  if [ -s "$logf" ]; then
    echo "---- wine console ($logf) ----"
    cat "$logf"
  fi
  if [ -f "$outf" ]; then
    echo "---- ${base}_out.txt ----"
    cat "$outf"
  else
    echo "[bench-wowsilicon] note: no ${base}_out.txt produced"
  fi
  [ "$rc" -eq 0 ] || echo "[bench-wowsilicon] exit status: $rc"
  return "$rc"
}

cmd_reset_runtime() {
  [ -d "$RT" ] || { echo "[bench-wowsilicon] no runtime copy to reset"; return 0; }
  echo "[bench-wowsilicon] removing runtime copy: $RT"
  env WINEPREFIX="$PFX" "$WINESERVER" -k >/dev/null 2>&1 || true
  rm -rf "$RT"
  copy_runtime
}

case "${1:-}" in
  install)        shift; cmd_install "$@" ;;
  run)            shift; [ $# -ge 1 ] || die "usage: $0 run <exe> [K=V ...]"; cmd_run "$@" ;;
  reset-runtime)  shift; cmd_reset_runtime "$@" ;;
  *)
    cat >&2 <<EOF
usage: $0 <command>
  install            stage build/ artifacts into the bench env
  run <exe> [K=V...] run one exe under the env (e.g. run bench.exe BENCH_MODE=vb)
  reset-runtime      force a fresh copy of the runtime from D9MT_RT_SRC
EOF
    exit 2 ;;
esac
