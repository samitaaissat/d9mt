/* depthbias: proves D3DRS_DEPTHBIAS carries D3D9 semantics — a raw offset
 * added to the fragment z (order 1e-4 in normalized depth), NOT the
 * Vulkan/Metal "least representable value" units the backend's
 * MTLRenderCommandEncoder setDepthBias expects. WoW's projected ground
 * textures re-draw terrain with a small negative constant bias; if the
 * bias arrives ~2^23 too small they z-fight (decals clip into terrain).
 *
 * Three independent bands, fixed-function XYZRHW quads, no shader blobs
 * (verifytri pattern):
 *
 * Band A (top third) — the original coarse-bias trio:
 *   1. green quad at z=0.5                       (base layer)
 *   2. red   quad at z=0.5+2e-6, DEPTHBIAS=-5e-4 -> wins under D3D9
 *      semantics (0.500002 - 0.0005 < 0.5), loses if the bias is nulled
 *   3. blue  quad at z=0.6, bias reset to 0      -> must lose (sanity:
 *      proves the depth test itself works, so a broken depth test can't
 *      masquerade as a pass)
 *
 * Band B (middle third) — MARGINAL constant bias at z=0.9. The Metal depth
 *   buffer is Depth32Float, whose least-representable bias unit is
 *   r = 2^(e-23) with e the exponent of the primitive's z — for the
 *   z in [0.5,1) that WoW terrain actually occupies, r = 2^-24. A constant
 *   pre-scale of 2^23 therefore delivers HALF the game-requested offset
 *   (and less as z shrinks below 0.5) — enough for band A's 25x-margin
 *   bias, but z-fighting territory for a game-scale marginal bias on a
 *   slope. This band is sized so that the full offset wins and a halved
 *   offset loses:
 *   4. green base at z=0.9
 *   5. red decal at z=0.9+2.25e-5, DEPTHBIAS=-3e-5:
 *        full  offset: 0.9000225 - 3.0e-5 = 0.8999925 < 0.9 -> red
 *        half  offset: 0.9000225 - 1.5e-5 = 0.9000075 > 0.9 -> green (FAIL)
 *
 * Band C (bottom third) — slope-scale pass-through (regression guard; the
 *   slope term is unitless in both APIs and must NOT get the constant
 *   term's format scale):
 *   6. green sloped base, z = 0.5 + 0.2*(x/W)  (dz/dx = 0.2/W per pixel)
 *   7. red   quad, same slope, z shifted +1e-4, SLOPESCALEDEPTHBIAS=-0.5:
 *        slope bias = -0.5 * 7.8125e-4 = -3.90625e-4; +1e-4 - 3.9e-4 < 0
 *        -> red wins iff the slope term is applied unscaled
 *
 * Probe pixels: (W/2, H/6), (W/2, H/2), (W/2, 5H/6). All three bands must
 * land their expected color for PASS. Results go to depthbias_out.txt;
 * PASS/FAIL also on stdout. Self-exits. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

struct Vertex {
  float x, y, z, rhw;
  DWORD color;
};
#define FVF_QUAD (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static FILE *g_out;
#define LOG(...)                                                               \
  do {                                                                         \
    fprintf(g_out, __VA_ARGS__);                                               \
    fputc('\n', g_out);                                                        \
    fflush(g_out);                                                             \
    printf(__VA_ARGS__);                                                       \
    fputc('\n', stdout);                                                       \
    fflush(stdout);                                                            \
  } while (0)
#define CHECK(expr)                                                            \
  do {                                                                         \
    HRESULT hr_ = (expr);                                                      \
    if (FAILED(hr_)) {                                                         \
      LOG("FAIL: %s -> 0x%08lx", #expr, (unsigned long)hr_);                   \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

/* banded quad at constant depth */
static void quad(struct Vertex v[6], float x0, float y0, float x1, float y1,
                 float z, DWORD color) {
  const struct Vertex tl = {x0, y0, z, 1.0f, color};
  const struct Vertex tr = {x1, y0, z, 1.0f, color};
  const struct Vertex bl = {x0, y1, z, 1.0f, color};
  const struct Vertex br = {x1, y1, z, 1.0f, color};
  v[0] = tl; v[1] = tr; v[2] = bl;
  v[3] = bl; v[4] = tr; v[5] = br;
}

/* banded quad with z varying linearly in x: z(x0)=z0, z(x1)=z1 */
static void slopeQuad(struct Vertex v[6], float x0, float y0, float x1,
                      float y1, float z0, float z1, DWORD color) {
  const struct Vertex tl = {x0, y0, z0, 1.0f, color};
  const struct Vertex tr = {x1, y0, z1, 1.0f, color};
  const struct Vertex bl = {x0, y1, z0, 1.0f, color};
  const struct Vertex br = {x1, y1, z1, 1.0f, color};
  v[0] = tl; v[1] = tr; v[2] = bl;
  v[3] = bl; v[4] = tr; v[5] = br;
}

static DWORD floatBits(float f) {
  DWORD d;
  memcpy(&d, &f, 4);
  return d;
}

int main(void) {
  const UINT W = 256, H = 258; /* 3 x 86-pixel bands */
  const float bandH = 86.0f;

  g_out = fopen("depthbias_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "depthbias";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("depthbias", "d9mt depthbias", WS_OVERLAPPEDWINDOW,
                            64, 64, W, H, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    LOG("FAIL: Direct3DCreate9");
    return 1;
  }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = W;
  pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;

  IDirect3DDevice9 *dev = NULL;
  HRESULT hr = IDirect3D9_CreateDevice(
      d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (FAILED(hr) || !dev) {
    LOG("FAIL: CreateDevice 0x%08lx", (unsigned long)hr);
    return 1;
  }

  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE));

  IDirect3DSurface9 *sysmem = NULL;
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));

  const DWORD biasZero = floatBits(0.0f);
  const DWORD biasCoarse = floatBits(-0.0005f);  /* band A: 25x margin */
  const DWORD biasMarginal = floatBits(-3e-5f);  /* band B: 1.33x margin */
  const DWORD slopeHalfNeg = floatBits(-0.5f);   /* band C */

  struct Vertex v[6];
  int frames = 0, verified = 0, ok = 0;
  DWORD prev[3] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL,
                                 D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                 D3DCOLOR_XRGB(16, 16, 16), 1.0f, 0));
    CHECK(IDirect3DDevice9_BeginScene(dev));
    CHECK(IDirect3DDevice9_SetFVF(dev, FVF_QUAD));

    /* ---- band A: y in [0, 86) — coarse-bias trio ---- */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZero));
    quad(v, 0.0f, 0.0f, (float)W, bandH, 0.5f, 0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasCoarse));
    quad(v, 0.0f, 0.0f, (float)W, bandH, 0.500002f, 0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZero));
    quad(v, 0.0f, 0.0f, (float)W, bandH, 0.6f, 0xFF0000FF);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    /* ---- band B: y in [86, 172) — marginal bias at z=0.9 ---- */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZero));
    quad(v, 0.0f, bandH, (float)W, 2.0f * bandH, 0.9f, 0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasMarginal));
    quad(v, 0.0f, bandH, (float)W, 2.0f * bandH, 0.9000225f, 0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    /* ---- band C: y in [172, 258) — slope-scale pass-through ---- */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZero));
    slopeQuad(v, 0.0f, 2.0f * bandH, (float)W, 3.0f * bandH, 0.5f, 0.7f,
              0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS,
                                          slopeHalfNeg));
    slopeQuad(v, 0.0f, 2.0f * bandH, (float)W, 3.0f * bandH, 0.5001f, 0.7001f,
              0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS,
                                          biasZero));

    CHECK(IDirect3DDevice9_EndScene(dev));

    /* Poll the readback every frame: async pipeline compilation
     * (D9MT_ASYNC / DXVK_ASYNC style) skips draws until the PSO is ready,
     * so early frames legitimately show only the clear color. Verify as
     * soon as SOMETHING drew; give up after ~600 frames. */
    if (++frames >= 3 && !verified) {
      IDirect3DSurface9 *bb = NULL;
      CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                                           D3DBACKBUFFER_TYPE_MONO, &bb));
      CHECK(IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem));
      IDirect3DSurface9_Release(bb);

      D3DLOCKED_RECT lr;
      CHECK(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY));
      const UINT probeY[3] = {43, 129, 215}; /* band centers */
      DWORD rgb[3];
      int i;
      for (i = 0; i < 3; i++)
        rgb[i] = (*(const DWORD *)((const BYTE *)lr.pBits +
                                   probeY[i] * lr.Pitch + (W / 2) * 4)) &
                 0x00FFFFFF;
      IDirect3DSurface9_UnlockRect(sysmem);

      /* Latch only after TWO consecutive frames agree on the full tuple:
       * the quads share PSOs and the async compile can flip one hot
       * between two draws of a single frame — that transition frame can
       * show any subset of the layers. The frame after the flip has every
       * draw live, so agreement across two frames screens the race out. */
      const int anyDrawn = rgb[0] != 0x00101010 || rgb[1] != 0x00101010 ||
                           rgb[2] != 0x00101010;
      const int stable = rgb[0] == prev[0] && rgb[1] == prev[1] &&
                         rgb[2] == prev[2];
      if (anyDrawn && stable) {
        verified = 1;
        LOG("probes: A=0x%06lx B=0x%06lx C=0x%06lx (frame %d)",
            (unsigned long)rgb[0], (unsigned long)rgb[1],
            (unsigned long)rgb[2], frames);
        int passA = rgb[0] == 0x00FF0000;
        int passB = rgb[1] == 0x00FF0000;
        int passC = rgb[2] == 0x00FF0000;
        LOG("%s: band A coarse constant bias (25x margin) %s", passA ? "ok" : "FAIL",
            passA ? "applied" : (rgb[0] == 0x0000FF00 ? "(decal lost to base layer)"
                                                      : "(BROKEN depth test)"));
        LOG("%s: band B marginal constant bias at z=0.9 (needs full 2^24 "
            "float-depth scale) %s", passB ? "ok" : "FAIL",
            passB ? "applied" : (rgb[1] == 0x0000FF00 ? "(underscaled: half-offset regime)"
                                                      : "(BROKEN depth test)"));
        LOG("%s: band C slope-scale bias pass-through %s", passC ? "ok" : "FAIL",
            passC ? "applied" : (rgb[2] == 0x0000FF00 ? "(slope term lost)"
                                                      : "(BROKEN depth test)"));
        if (passA && passB && passC) {
          LOG("PASS: DEPTHBIAS applied with D3D9 raw-offset semantics");
          ok = 1;
        } else {
          LOG("FAIL: depth bias semantics diverge from D3D9 (see bands above)");
        }
      } else if (frames >= 600) {
        verified = 1;
        LOG("FAIL: no stable frame in %d frames (pipeline never ready?)",
            frames);
      } else {
        prev[0] = rgb[0];
        prev[1] = rgb[1];
        prev[2] = rgb[2];
      }
    }

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    if (verified && frames >= 5)
      break;
  }

done:
  fclose(g_out);
  return ok ? 0 : 1;
}
