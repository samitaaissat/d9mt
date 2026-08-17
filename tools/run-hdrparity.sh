#!/usr/bin/env bash
# Numeric parity gate for the HDR present shader.
#
# Extracts the BT.2446-A fragment from d9mt_presenter.cpp's embedded MSL and
# runs it against a verbatim copy of mtld3d's reference fragment on the GPU,
# with the same MTLMathModeFast the driver uses. Any drift fails.
#
# Host-native: needs Metal and clang, NOT wine, a prefix or a game.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/d9mt-hdrparity"
mkdir -p "$OUT"

python3 - "$ROOT" "$OUT" <<'PY'
import sys
root, out = sys.argv[1], sys.argv[2]
s = open(f"{root}/src/d3d9fe/d9mt_presenter.cpp").read()
marker = 'const char g_blitShaderMsl[] = R"('
st = s.index(marker) + len(marker)
en = s.index(')";', st)
open(f"{out}/blit.metal", "w").write(s[st:en])
PY

clang++ -std=c++17 -ObjC++ -O2 "$ROOT/test/hdrparity.mm" \
  -framework Metal -framework Foundation -o "$OUT/hdrparity"
exec "$OUT/hdrparity" "$OUT/blit.metal"
