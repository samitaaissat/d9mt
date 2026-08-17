#!/usr/bin/env bash
# ABAB interleave of HDR-off vs HDR-on with the SAME binaries (env var only),
# so any delta is the HDR present path PLUS the cost of the compositor actually
# running the layer in EDR mode. The second part is the unknown: the tone map
# itself measured ~+1.5% at a forced peak, but a run where macOS genuinely
# promoted the screen to EDR mid-way (headroom 1.2x -> 2.53x) showed +53%. Those
# two disagree by 35x and neither is trustworthy - both were short runs on a host
# at load 20-75.
#
# RUN THIS ON AN IDLE MACHINE. Check `sysctl -n vm.loadavg` is under ~6 first,
# and sanity-check that p99_ms clusters tightly across runs (roughly within 10%
# of med_ms); if p99 is 2x+ the median, the host was busy and the numbers are
# worthless. Interleaving means load drift shows up as a sign flip between
# pairs rather than a fake win.
#
#   bash tools/ab-hdr-cost.sh            # 4 pairs, 150 frames, BENCH_RT=256
#   PAIRS=8 bash tools/ab-hdr-cost.sh    # more pairs = better resolution
#
# Requires `tools/bench-wowsilicon.sh install` to have been run first.
set -euo pipefail
ROOT=/Users/sami.taaissat/Documents/Perso/WoWSilicon/.d9mt-work/d9mt
SCR=/private/tmp/claude-502/-Users-sami-taaissat-Documents-Perso-WoWSilicon/9caaa54f-f0a1-4411-ba6c-07bb2e6ea56d/scratchpad
export D9MT_RT_SRC="/Volumes/Perso/WoWSilicon Data/RuntimeUpdate/WoWSilicon Game.app"
export D9MT_BENCH_ENV="$SCR/bench-env"
PAIRS="${PAIRS:-4}"
ARGS="${ARGS:-BENCH_MODE=rt BENCH_RT=256 BENCH_RTDRAWS=4 BENCH_FRAMES=150}"
cd "$ROOT"
one() {
  "$D9MT_BENCH_ENV/WoWSilicon Game.app/Contents/MacOS/wineserver" -k 2>/dev/null || true
  sleep 1
  local extra=""
  [ "$1" = "ON" ] && extra="D9MT_HDR=1"
  echo "$1 $(bash tools/bench-wowsilicon.sh run bench.exe $extra $ARGS 2>&1 | grep '^RESULT' || echo 'RESULT FAILED')"
}
for i in $(seq 1 "$PAIRS"); do one OFF; one ON; done
