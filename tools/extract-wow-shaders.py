#!/usr/bin/env python3
"""Extract D3D9 shader bytecode from a WoW 3.3.5 client for test use.

Pulls the named .bls shader containers out of the client MPQs (respecting
patch override order), slices the raw DXSO blobs out of them (version token
0xFFFE/0xFFFF xxxx .. END 0x0000FFFF), and writes .vso/.pso files.

The blobs are game content and MUST NOT be committed to this repo — the
shader tests that embed them (test/spectest.c) are skipped by scripts/
build.sh when the blobs are absent, same as the other bytecode tests.

usage: extract-wow-shaders.py <game-dir> <out-dir> [name-substring ...]
       (default substrings: terrain, shadowmap, mapobjspecular)

requires: pip install mpyq
"""
import glob
import os
import struct
import sys

from mpyq import MPQArchive

VS_TOKENS = (0xFFFE0101, 0xFFFE0200, 0xFFFE0300)
PS_TOKENS = (0xFFFF0101, 0xFFFF0104, 0xFFFF0200, 0xFFFF0300)
PROFILE = {
    0xFFFE0101: "vs11", 0xFFFE0200: "vs20", 0xFFFE0300: "vs30",
    0xFFFF0101: "ps11", 0xFFFF0104: "ps14", 0xFFFF0200: "ps20",
    0xFFFF0300: "ps30",
}


def slice_blobs(raw):
    n = len(raw) // 4
    dwords = struct.unpack("<%dI" % n, raw[: n * 4])
    out = []
    for i, d in enumerate(dwords):
        if d not in PROFILE:
            continue
        for j in range(i + 1, n):
            if dwords[j] == 0x0000FFFF:
                out.append((PROFILE[d], raw[i * 4 : (j + 1) * 4]))
                break
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    game_dir, out_dir = sys.argv[1], sys.argv[2]
    wanted = [w.lower().encode() for w in (sys.argv[3:] or
              ["terrain", "shadowmap", "mapobjspecular"])]
    os.makedirs(out_dir, exist_ok=True)

    mpqs = sorted(glob.glob(os.path.join(game_dir, "Data", "*.MPQ")))
    effective = {}
    for path in mpqs:  # sorted order approximates patch priority
        try:
            archive = MPQArchive(path, listfile=True)
        except Exception:
            continue
        for f in archive.files or []:
            fl = f.lower()
            if not fl.endswith(b".bls"):
                continue
            if not (fl.startswith(b"shaders\\pixel\\") or
                    fl.startswith(b"shaders\\vertex\\")):
                continue
            if not any(w in fl for w in wanted):
                continue
            effective[fl] = (path, f)

    count = 0
    archives = {}
    for key, (path, orig) in sorted(effective.items()):
        if path not in archives:
            archives[path] = MPQArchive(path, listfile=True)
        data = archives[path].read_file(orig)
        if not data:
            continue
        base = key.decode(errors="replace").replace("\\", "_")[:-4]
        for si, (prof, blob) in enumerate(slice_blobs(data)):
            ext = "vso" if prof.startswith("vs") else "pso"
            out = os.path.join(out_dir, "%s.%s.%d.%s" % (base, prof, si, ext))
            with open(out, "wb") as fh:
                fh.write(blob)
            count += 1
    print("extracted %d shader blobs to %s" % (count, out_dir))


if __name__ == "__main__":
    main()
