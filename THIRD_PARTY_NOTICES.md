# Third-Party Notices

This file records third-party code incorporated into d9mt, together with the
licence terms that travel with it. It is a compliance record, not a summary:
where a licence requires verbatim reproduction, the text is reproduced in
full below.

No pre-existing `THIRD_PARTY_NOTICES.md` was present when this file was
created; nothing was replaced.

> **UNRESOLVED LICENCE CONFLICT — see [Lilium](#2-liliums-reshade-hdr-shaders-gpl-30--unresolved).**
> The BT.2446-A inverse tone-mapping work described below traces back to a
> **GPL-3.0** upstream. That conflict is *not* discharged by the zlib notice in
> section 1 and is **not** resolved by this file. Read section 2 before
> landing any BT.2446 shader code.

---

## 1. mtld3d

- **Upstream:** https://github.com/athei/mtld3d
- **Version:** v0.6.0 @ `94d567ef8772023dc70157c40547daa515e8eb7c`
- **Author:** Alexander Theissen (the repository LICENSE spells the name
  "Theissen"; git commit authorship spells it "Alexander Theißen")
- **Licence:** zlib

### Files in d9mt derived from mtld3d

| d9mt file | mtld3d origin |
| --- | --- |
| `src/d3d9fe/d9mt_presenter.cpp` | `unix/unix/src/metal/present.msl` (the BT.2446-A/ICtCp fragment) and the host-side pipeline/uniform logic in `unix/unix/src/metal/{present,command}.rs` |
| `src/d9mtmetal/unix.m`, `src/d9mtmetal/d9mtmetal.h` | the EDR-headroom publish design and 32-present refresh cadence in `unix/unix/src/metal/{macdrv,command}.rs` |

**Status note (accuracy):** on branch `feat/hdr-bt2446` the ported BT.2446-A
code lives in `src/d3d9fe/d9mt_presenter.cpp` as an **embedded Metal Shading
Language source string**, with supporting host-side derivation in
`src/d9mtmetal/d9mtmetal.h` and `src/d9mtmetal/unix.m` (both now carry their own
mtld3d attribution headers). These are **committed** as of `c8f976e`
("feat: HDR present pipeline") and its follow-ups; the table above is current
for that branch. Nothing has been pushed to a remote, and no payload containing
this code has been uploaded to a release.

### zlib licence — complete verbatim text

The following is reproduced verbatim from `LICENSE` at mtld3d
`94d567ef8772023dc70157c40547daa515e8eb7c`:

```
Copyright (c) 2026 Alexander Theissen

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.
```

### ALTERED SOURCE STATEMENT (zlib restriction 2)

**This is an altered source version of mtld3d. It is not the original
software.**

The material incorporated into d9mt has been modified from Alexander
Theissen's original. Alterations include, but are not limited to:

- translation from Rust + Metal Shading Language (`present.rs`,
  `present.msl`) into d9mt's C++ presenter (`src/d3d9fe/d9mt_presenter.cpp`),
- adaptation from mtld3d's own Metal presenter to d9mt's DXVK-derived
  presenter and swapchain-blitter architecture,
- changes to uniform plumbing, pipeline construction, and the CPU-side
  selection between the copy, passthrough, and BT.2446 present routes.

Alexander Theissen has not reviewed, endorsed, or approved these
alterations, and must not be represented as the author of them. Bugs in
d9mt's version are d9mt's, not upstream's.

Per restriction 3, the notice above may not be removed or altered from any
source distribution of d9mt that carries this material.

---

## 2. Lilium's ReShade HDR shaders (GPL-3.0 — UNRESOLVED)

- **Upstream:** https://github.com/EndlesslyFlowering/ReShade_HDR_shaders
- **Author:** "Lilium" (GitHub user `EndlesslyFlowering`)
- **File of interest:** `Shaders/lilium__include/inverse_tone_mappers.fxh`
- **Licence:** **GNU General Public License v3.0** (`LICENSE` in the
  repository root: "GNU GENERAL PUBLIC LICENSE, Version 3, 29 June 2007").
  The repository has carried this licence since the `LICENSE` file was added
  on 2023-06-27 (commit `553f3ab4c`) — it is the only commit ever to touch
  that file, so **there is no earlier, more permissive version to fall back
  on** for any code taken after that date. `inverse_tone_mappers.fxh` carries
  no per-file header, no SPDX identifier and no separate grant, so it
  inherits the repository licence.

### Why this section exists

mtld3d's `present.rs` states, in its own words:

> "Ported from the `ICtCp` branch of `Bt2446A` in Lilium's `ReShade` HDR
> shaders (`Shaders/lilium__include/inverse_tone_mappers.fxh`)."

and `present.msl` additionally credits Lilium inline for the colour matrices
and for the chroma-preserving pattern. **mtld3d ships no Lilium licence text
and no NOTICE file — only its own root LICENSE (zlib).** A zlib grant from
Alexander Theissen cannot license out Lilium's copyright; an upstream cannot
grant more than it holds. d9mt therefore cannot rely on section 1 to cover
the Lilium-derived material.

### What GPL-3.0 actually requires

GPL-3.0 is a strong copyleft licence. For a work that is a derivative of
GPL-3.0 code and is conveyed to others, the operative obligations are:

- **§4 / §5 — licence the whole:** the derivative work must be released
  "as a whole" under GPL-3.0 to anyone who receives it. This is not
  satisfiable by attribution alone.
- **§5(a)(b) — notices:** the modified work must carry prominent notices
  stating that it is modified, the date of change, and that it is released
  under GPL-3.0.
- **§6 — corresponding source:** conveying the work in binary form obliges
  you to provide the complete corresponding source under GPL-3.0.
- **§7 — no additional restrictions:** you may not impose terms that
  restrict the rights GPL-3.0 grants.
- **Notice retention:** all copyright and licence notices must be preserved.

So: attribution is **necessary but nowhere near sufficient**. GPL-3.0 is
copyleft with a source-disclosure obligation, and it is incompatible with
shipping the affected code under zlib.

### Scope of the exposure — narrower than the provenance note suggests

This was checked against Lilium's upstream at `master`, line by line. Two
findings materially narrow the exposure, and one preserves it:

1. **The "ICtCp branch of `Bt2446A`" does not exist upstream.** Lilium's
   `Bt2446A` has exactly two processing modes,
   `BT2446A_PRO_MODE_LUMINANCE` and `BT2446A_PRO_MODE_YCBCR_LIKE` — neither
   operates in ICtCp. The only ICtCp-based inverse tone mapper in that file is
   a *separate and different* algorithm, `Itmos::Dice::InverseToneMapper`,
   which expands luminance with a shoulder-start early-out and corrects chroma
   by `min_I = min(min(I1/I2, I2/I1) * 1.1, 1)`. d9mt/mtld3d instead scale
   Ct/Cp by `i_hdr / i_in`. **The ICtCp wrapper is therefore not Lilium's
   code.** The description inherited from mtld3d is inaccurate on this point.

2. **The matrices are not Lilium's values.** mtld3d's and d9mt's BT.709→LMS
   matrix (`0.2958197875977, …`) does *not* match Lilium's
   `BT709_To_ICtCp_LMS` (`0.295764088, …`), and the ICtCp→LMS-PQ inverse
   (`0.00860903, …`) does not match Lilium's (`0.00860647484, …`). They differ
   in the 4th–5th significant digit, which is consistent with independent
   derivation. The *only* matrix that matches Lilium exactly is the forward
   LMS-PQ→ICtCp matrix — and that is the verbatim published BT.2100 table,
   which Lilium itself took from the standard. mtld3d's inline claim that the
   values come "from Lilium's `colour_space.fxh`" is not borne out; d9mt's own
   header correctly attributes them to BT.2100 / ST.2084 instead.

3. **What does remain is the luminance curve inversion.** These four lines in
   `d9mt_bt2446a_ictcp` correspond line-for-line to Lilium's `Bt2446A`,
   including the folded literal `4.83307641f`, the `4.604f` and `/ -2.302f`
   forms, and the three-way branch ordering:

   ```c
   float yp_0 = yp_c / 1.0770f;
   float yp_1 = (-2.7811f + sqrt(4.83307641f - 4.604f * yp_c)) / -2.302f;
   float yp_2 = (yp_c - 0.5f) / 0.5f;
   float yp_p = yp_0 <= 0.7399f ? yp_0 : (yp_2 >= 0.9909f ? yp_2 : yp_1);
   ```

So the GPL-3.0 exposure is concentrated in roughly four lines, not in the
shader as a whole. See the copyrightability assessment for why that is
genuinely arguable in both directions — and why the documented "ported from"
provenance is the harder problem than the constants themselves.

### Current status in d9mt

**UNRESOLVED. This is a blocker, not a formality.** d9mt vendors DXVK and
spirv-cross (both permissive) and its README states that "d9mt's own code
follows suit". Introducing GPL-3.0-derived code into that tree would place
the combined work under GPL-3.0 and contradict the project's stated
licensing. No Lilium-derived code should be conveyed until this is resolved
by one of the routes in the compliance checklist accompanying this file.

---

## 3. Standards-derived constants (ITU-R / SMPTE)

Some of the numeric material in the HDR present path is not authored
expression belonging to any of the parties above. Specifically:

- **ITU-R BT.2100** — the ICtCp transform matrices and the definition of the
  L'M'S' → ICtCp mapping (Table 5 / §3.4).
- **ITU-R BT.2446 (Method A)** — the SDR→HDR conversion equations and the
  `p = 1 + 32·(L/10000)^(1/2.4)` parameterisation, including the published
  three-segment forward curve.
- **SMPTE ST.2084 / BT.2100** — the PQ EOTF and inverse-EOTF constants
  (`m1 = 2610/16384`, `m2 = 2523/4096·128`, `c1 = 3424/4096`,
  `c2 = 2413/4096·32`, `c3 = 2392/4096·32`).
- **IEC 61966-2-1** — the piecewise sRGB EOTF (`0.04045`, `12.92`, `1.055`,
  `0.055`, `2.4`).
- **BT.709** — the luminance weights `(0.2126, 0.7152, 0.0722)`.

These are published constants, matrices and equations from technical
standards. They are facts and mathematical relationships rather than creative
expression, and reproducing them does not, by itself, create an obligation to
any third-party software project. Implementing them from the standards
documents directly is a clean path.

**This does not extend to a particular *implementation* of those equations.**
Where a specific arrangement, algebraic folding, naming scheme or branch
ordering is copied from another project's source, the obligation attaches to
that project's licence regardless of the standards origin of the underlying
mathematics. See section 2.

Note also that the standards documents themselves are copyrighted
publications of the ITU/SMPTE/IEC; this notice concerns software licensing,
not redistribution of the standards texts.

---

## 4. Other vendored components

These predate this file and are recorded here for completeness. Their terms
are not analysed above and their licence files are the authority:

- **DXVK** — https://github.com/doitsujin/dxvk, vendored at `vendor/dxvk`
  (zlib upstream). **No licence file is currently present under
  `vendor/dxvk/`** — see the compliance checklist.
- **spirv-cross** — vendored at `vendor/spirv-cross`; licence text present at
  `vendor/spirv-cross/LICENSE`.
