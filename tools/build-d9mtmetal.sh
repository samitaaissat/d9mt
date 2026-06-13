#!/usr/bin/env bash
# Build the d9mtmetal companion unixlib: Mach-O .so (unix side) plus
# 32/64-bit builtin-marked PE dlls, with import libs generated from
# CrossOver's own ntdll.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/d9mtmetal"
OUT="$ROOT/build"
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
mkdir -p "$OUT/d9mtmetal"

echo "[d9mtmetal] unix .so (x86_64: wine's unix side runs under Rosetta)"
clang -ObjC -dynamiclib -arch x86_64 -O2 \
  -o "$OUT/d9mtmetal/d9mtmetal.so" \
  "$SRC/unix.m" \
  -I "$SRC" \
  -install_name @rpath/d9mtmetal.so \
  -lsqlite3 \
  -framework Metal -framework Foundation

echo "[d9mtmetal] ntdll import libs from CrossOver's ntdll exports"
# 32-bit: stdcall-decorated import names, -k strips the @N from the
# import table (wine's ntdll exports undecorated names)
cat > "$OUT/d9mtmetal/ntdll-cx32.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
  __wine_unix_call@16
  NtQueryVirtualMemory@24
EOF
i686-w64-mingw32-dlltool -k -d "$OUT/d9mtmetal/ntdll-cx32.def" \
  -l "$OUT/d9mtmetal/libntdll-cx32.a" --dllname ntdll.dll
cat > "$OUT/d9mtmetal/ntdll-cx64.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
  __wine_unix_call
  NtQueryVirtualMemory
EOF
x86_64-w64-mingw32-dlltool -d "$OUT/d9mtmetal/ntdll-cx64.def" \
  -l "$OUT/d9mtmetal/libntdll-cx64.a" --dllname ntdll.dll

echo "[d9mtmetal] PE dlls"
# --file-alignment must match section alignment: wine maps builtin PEs
# as flat files (this is what winebuild --builtin produces).
# -s strips debug sections: file size must not exceed SizeOfImage or
# wine's server/mapping.c returns SECTION_TOO_BIG and silently skips
# the builtin (then no unixlib pairing).
# build under the REAL name in per-arch dirs: the PE export table's
# internal name comes from the output filename and wine matches builtin
# identity against it
mkdir -p "$OUT/d9mtmetal/i386" "$OUT/d9mtmetal/x86_64"
i686-w64-mingw32-gcc -shared -O2 -s -o "$OUT/d9mtmetal/i386/d9mtmetal.dll" \
  "$SRC/dll.c" -I "$SRC" "$OUT/d9mtmetal/libntdll-cx32.a" \
  -Wl,--file-alignment,0x1000
x86_64-w64-mingw32-gcc -shared -O2 -s -o "$OUT/d9mtmetal/x86_64/d9mtmetal.dll" \
  "$SRC/dll.c" -I "$SRC" "$OUT/d9mtmetal/libntdll-cx64.a" \
  -Wl,--file-alignment,0x1000
cp "$OUT/d9mtmetal/i386/d9mtmetal.dll" "$OUT/d9mtmetal/d9mtmetal32.dll"
cp "$OUT/d9mtmetal/x86_64/d9mtmetal.dll" "$OUT/d9mtmetal/d9mtmetal64.dll"

echo "[d9mtmetal] builtin markers"
python3 "$ROOT/tools/make-builtin.py" "$OUT/d9mtmetal/d9mtmetal32.dll"
python3 "$ROOT/tools/make-builtin.py" "$OUT/d9mtmetal/d9mtmetal64.dll"

echo "[d9mtmetal] import lib for d3d9.dll to link against"
cat > "$OUT/d9mtmetal/d9mtmetal.def" <<'EOF'
LIBRARY d9mtmetal.dll
EXPORTS
  D9MT_UnixCall
EOF
i686-w64-mingw32-dlltool -d "$OUT/d9mtmetal/d9mtmetal.def" \
  -l "$OUT/d9mtmetal/libd9mtmetal32.a" --dllname d9mtmetal.dll

echo "[d9mtmetal] installing into CrossOver dxmt dir (in WINEDLLPATH with"
echo "            proper arch layout; this is how CX26's bundled DXMT ships)"
mkdir -p "$CX/lib/dxmt/i386-windows" "$CX/lib/dxmt/x86_64-windows" "$CX/lib/dxmt/x86_64-unix"
cp "$OUT/d9mtmetal/d9mtmetal32.dll" "$CX/lib/dxmt/i386-windows/d9mtmetal.dll"
cp "$OUT/d9mtmetal/d9mtmetal64.dll" "$CX/lib/dxmt/x86_64-windows/d9mtmetal.dll"
cp "$OUT/d9mtmetal/d9mtmetal.so"    "$CX/lib/dxmt/x86_64-unix/d9mtmetal.so"

echo "[d9mtmetal] fallback: wine tree arch dirs (WINEDLLPATH entries are the"
echo "            arch dirs themselves, so the .so must sit NEXT to the PE"
echo "            for find_builtin_dll's bare-path probe to pair it)"
cp "$OUT/d9mtmetal/d9mtmetal32.dll" "$CX/lib/wine/i386-windows/d9mtmetal.dll"
cp "$OUT/d9mtmetal/d9mtmetal.so"    "$CX/lib/wine/i386-windows/d9mtmetal.so"
cp "$OUT/d9mtmetal/d9mtmetal64.dll" "$CX/lib/wine/x86_64-windows/d9mtmetal.dll"
cp "$OUT/d9mtmetal/d9mtmetal.so"    "$CX/lib/wine/x86_64-windows/d9mtmetal.so"
cp "$OUT/d9mtmetal/d9mtmetal.so"    "$CX/lib/wine/x86_64-unix/d9mtmetal.so"

echo "[d9mtmetal] installing prefix copies (PE search must find the file"
echo "            to trigger wine's builtin load path; see loader.c"
echo "            find_builtin_without_file: WINEDLLPATH-only lookup works"
echo "            solely during prefix bootstrap)"
BOTTLE_WIN="$HOME/Library/Application Support/CrossOver/Bottles/Rockstar Games Launcher/drive_c/windows"
cp "$OUT/d9mtmetal/d9mtmetal32.dll" "$BOTTLE_WIN/syswow64/d9mtmetal.dll"
cp "$OUT/d9mtmetal/d9mtmetal64.dll" "$BOTTLE_WIN/system32/d9mtmetal.dll"

echo "[d9mtmetal] registering builtin override in bottle"
"$CX/bin/wine" --bottle "Rockstar Games Launcher" reg add \
  'HKCU\Software\Wine\DllOverrides' /v d9mtmetal /d builtin /f \
  >/dev/null 2>&1 || true

echo "[d9mtmetal] done"
