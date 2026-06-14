# Running GTA IV on d9mt (Apple silicon, CrossOver)

This walks through getting **Grand Theft Auto IV** booting on the d9mt driver
under CrossOver on Apple silicon.
```
GTAIV.exe -> d3d9.dll (d9mt's d3d9fe.dll) -> winemetal -> Metal
```

> **You must own the game.** This repo ships no game files, no patched
> executables, and no copyrighted binaries. Supply your own legally-obtained
> copy. The steps below only describe configuration you apply to *your* install.

GTA IV is a 32-bit, CPU-heavy game; under Rosetta 2 it is CPU-bound. Expect
playable-but-not-maxed framerates.

---

## 1. Remove the Games-for-Windows-Live dependency

Stock GTA IV launches through **`PlayGTAIV.exe`**, which initializes
Games-for-Windows-Live (GFWL). Under wine that path null-derefs on startup
(crash inside `PlayGTAIV.exe` *before* `d3d9.dll` ever loads — so it looks like
a driver bug but isn't). You must neutralize GFWL and launch the game exe
directly.

Use the community **xliveless** GFWL stub (a.k.a. `xlive.dll` replacement /
`xlivelessaddon`). It is a free, widely-distributed third-party mod that
satisfies the `xlive.dll` import without GFWL. Install per its own
instructions into the game folder (next to `GTAIV.exe`).

After installing it, confirm:
- `xlive.dll` (the stub) sits in the game directory, **and**
- you launch **`GTAIV.exe` directly** — *not* `PlayGTAIV.exe`. The
  `play-gta4.sh` script in this folder does exactly that.


```
mv movies/rockstar_logos.bik movies/rockstar_logos.bik.bak   # etc.
```

## 2. CrossOver bottle settings

- A 64-bit bottle (Windows 7 or 10) works.
- Graphics: default/Auto. msync on is fine.
- The d9mt driver replaces `d3d9.dll` only — leave the rest of the bottle stock.

## 3. Build the driver

From the repo root (see the top-level README for prerequisites):

```bash
bash scripts/build-dxvkfe.sh            # dev build (logging + trace)
# or, for a release build (max speed, no logging/trace):
RELEASE=1 bash scripts/build-dxvkfe.sh
```

> **If you edit any header**, do a clean rebuild — the build does not track
> header dependencies and will otherwise relink a stale object:
> `rm -rf build/dxvkfe-obj build/dxvkfe-obj-release`

## 4. Deploy + launch

Point the script at your install and run it:

```bash
BOTTLE_NAME="GTA IV" \
GTA4_GAME_DIR="$HOME/Library/Application Support/CrossOver/Bottles/GTA IV/drive_c/Program Files (x86)/Rockstar Games/Grand Theft Auto IV" \
./examples/play-gta4.sh
```

It copies `build/d3d9fe.dll` over the game's `d3d9.dll` (saving the original to
`d3d9.dll.pre-d9mt-bak` once), then launches `GTAIV.exe`.

Useful flags:

| flag          | effect                                            |
|---------------|---------------------------------------------------|
| `--no-deploy` | launch without re-copying the driver              |
| `--hud`       | Metal performance HUD overlay                     |
| `--log`       | follow `d3d9fe.log` after launch                  |
| `--restore`   | put back the original (pre-d9mt) `d3d9.dll`       |
| `--kill`      | kill a stuck game                                 |

First boot compiles shaders (~1–2 min); subsequent boots hit the on-disk
shader cache and start fast. The driver log is `d3d9fe.log` in the game folder.

## Troubleshooting

- **Crash in `PlayGTAIV.exe` / `playgtaiv+0x….` before any d3d9 log** — GFWL
  not removed, or you launched `PlayGTAIV.exe`. Redo step 1, launch `GTAIV.exe`.
