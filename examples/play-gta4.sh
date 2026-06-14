#!/usr/bin/env bash
#
# play-gta4.sh — run GTA IV on the d9mt driver (native D3D9 -> Metal, DXVK
# front-end + d9mt's Metal backend, NO Vulkan/MoltenVK in the path).
#
#   GTAIV.exe -> d3d9.dll (= d9mt's d3d9fe.dll) -> winemetal -> Metal
#
# Deploys the built driver as the game's d3d9.dll and launches it through
# CrossOver's bundled wine. Tested with GTA IV (Complete Edition, 1.0.7.0)
# under CrossOver on Apple silicon.
#
# Paths are overridable by env var — set these for your install:
#   CX_APP        CrossOver.app location (default /Applications/CrossOver.app)
#   BOTTLE_NAME   CrossOver bottle the game lives in
#   GTA4_GAME_DIR folder containing GTAIV.exe + d3d9.dll
#   D9MT_REPO     this repo (default: parent of examples/)
#
# Usage:
#   ./play-gta4.sh             deploy build/d3d9fe.dll + launch
#   ./play-gta4.sh --no-deploy launch without re-copying the driver
#   ./play-gta4.sh --hud       Metal performance HUD overlay
#   ./play-gta4.sh --log       follow the driver log after launch
#   ./play-gta4.sh --restore   put back the original d3d9.dll (pre-d9mt)
#   ./play-gta4.sh --kill      kill a stuck game
#
# NOTE: you must own GTA IV and supply your own game files. This script ships
# no game binaries. GTA IV's Games-for-Windows-Live dependency must be removed
# first (see examples/GTA-IV-SETUP.md) or the game will not boot under wine.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${D9MT_REPO:-$(cd "$SCRIPT_DIR/.." && pwd)}"

CX_APP="${CX_APP:-/Applications/CrossOver.app}"
CX_WINE="$CX_APP/Contents/SharedSupport/CrossOver/bin/wine"
BOTTLE_NAME="${BOTTLE_NAME:-GTA IV}"
BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE_NAME"
GAME_DIR="${GTA4_GAME_DIR:-$BOTTLE/drive_c/Program Files (x86)/Rockstar Games/Grand Theft Auto IV}"

ORIG_BAK="d3d9.dll.pre-d9mt-bak"   # original d3d9.dll, saved on first deploy

c_grn=$'\033[32m'; c_red=$'\033[31m'; c_rst=$'\033[0m'
ok()  { printf '%s✓%s %s\n' "$c_grn" "$c_rst" "$*"; }
die() { printf '%s✗ %s%s\n' "$c_red" "$*" "$c_rst" >&2; exit 1; }

[[ -x "$CX_WINE" ]]            || die "CrossOver wine not found at $CX_WINE (set CX_APP)"
[[ -f "$GAME_DIR/GTAIV.exe" ]] || die "GTAIV.exe not found in $GAME_DIR (set GTA4_GAME_DIR)"

HUD=0 DEPLOY=1 FOLLOW=0 ACTION=launch
while [[ $# -gt 0 ]]; do
  case "$1" in
    --deploy)     DEPLOY=1 ;;
    --no-deploy)  DEPLOY=0 ;;
    --hud)        HUD=1 ;;
    --log)        FOLLOW=1 ;;
    --restore)    ACTION=restore ;;
    --kill)       ACTION=kill ;;
    -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
    *) die "unknown arg: $1" ;;
  esac
  shift
done

if [[ "$ACTION" == kill ]]; then
  pkill -9 -f "GTAIV.exe" 2>/dev/null || true
  ok "killed"
  exit 0
fi

if [[ "$ACTION" == restore ]]; then
  [[ -f "$GAME_DIR/$ORIG_BAK" ]] || die "no backup ($ORIG_BAK) — nothing to restore"
  cp "$GAME_DIR/$ORIG_BAK" "$GAME_DIR/d3d9.dll"
  ok "restored original d3d9.dll"
  exit 0
fi

if [[ "$DEPLOY" == 1 ]]; then
  [[ -f "$REPO/build/d3d9fe.dll" ]] || die "no build/d3d9fe.dll — run scripts/build-dxvkfe.sh first"
  # preserve the game's original d3d9.dll once, so --restore always works
  if [[ -f "$GAME_DIR/d3d9.dll" && ! -f "$GAME_DIR/$ORIG_BAK" ]] \
     && ! cmp -s "$REPO/build/d3d9fe.dll" "$GAME_DIR/d3d9.dll"; then
    cp "$GAME_DIR/d3d9.dll" "$GAME_DIR/$ORIG_BAK"
    ok "saved original d3d9.dll -> $ORIG_BAK"
  fi
  cp "$REPO/build/d3d9fe.dll" "$GAME_DIR/d3d9.dll"
  ok "deployed $(ls -la "$REPO/build/d3d9fe.dll" | awk '{print $5}') bytes -> d3d9.dll"
fi

declare -a ENVV=(WINEDLLOVERRIDES="d3d9=n")
[[ "$HUD" == 1 ]] && ENVV+=(MTL_HUD_ENABLED=1)

cd "$GAME_DIR"
rm -f d3d9fe.log
ok "launching GTAIV.exe (d9mt Metal driver)…"
env "${ENVV[@]}" "$CX_WINE" --bottle "$BOTTLE_NAME" start /unix "$GAME_DIR/GTAIV.exe" \
  > /tmp/gta4-d9mt-launch.log 2>&1 || die "wine launch failed (see /tmp/gta4-d9mt-launch.log)"
ok "launched (driver log: d3d9fe.log in game dir; first boot compiles shaders ~1-2 min)"

if [[ "$FOLLOW" == 1 ]]; then
  touch d3d9fe.log
  exec tail -f d3d9fe.log
fi
