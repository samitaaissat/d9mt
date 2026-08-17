#!/usr/bin/env bash
# ABAB interleave of HDR-off vs HDR-on with the SAME binaries (env var only),
# so any delta is the HDR present path plus whatever the compositor costs when
# it actually runs the layer in EDR mode.
#
# RESULT (2026-08-17, 6 pairs, rt 256): the cost is essentially ZERO.
#   med_ms 18.407 off vs 18.381 on  (-0.1%), p99 within 5% of median for both.
# Agrees with a +1.5% forced-peak measurement and with the structural argument
# that the tone map adds no pass (WoW's X8R8G8B8 swizzle already forces present
# down the sample-pass path).
#
# HOW THIS RUN NEARLY LIED, because it will lie again on a busy host: the first
# four pairs showed HDR-on medians of 34-53 ms against an off-minimum of 20.7,
# an apparent +67%, and a separate single run showed 6.67 -> 10.20 ms (+53%).
# All of it was host contamination and it was reported as a real framerate trade
# before the quiet pairs arrived. min-of-N IS load-robust in principle (noise is
# additive) but only once EVERY variant has found a quiet window, and at that
# point only the off side had.
#
# RUN THIS ON AN IDLE MACHINE. Require `sysctl -n vm.loadavg` under ~6, and
# require p99_ms within ~10% of med_ms PER RUN — that is the tell that was
# missing from the misleading rounds (p99 was 3-12x the median in every one of
# them, within 5% in the truthful ones). Discard any run that fails it no matter
# how plausible the number looks, and never compare minima until both sides have
# a tight-p99 run. Even the '20.4-21.1 clean baseline' used earlier on this
# branch was somewhat loaded; the real floor is 18.4.
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
