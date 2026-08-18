/* depthbias: pins down d9mt's depth-bias semantics on Apple float-depth.
 *
 * Ground truth this test encodes (verified on-device 2026-08-18, and
 * matching the Vulkan float-depth spec): Metal applies the setDepthBias
 * constant in units r = 2^(e-23), where e is the exponent of the
 * primitive's z. The front-end pre-scales the raw D3D9 bias by 2^23
 * (stock DXVK fallback), so the EFFECTIVE window-z offset is raw * 2^e:
 * raw/2 for z in [0.5,1), raw/4 in [0.25,0.5), and so on. That octave
 * dependence is inherent to float depth — a constant pre-scale cannot
 * remove it (force-unorm advertisement just moves the correct octave and
 * violates the representation's constancy promise; band D history).
 *
 * WoW itself never sets DEPTHBIAS in gameplay (client disassembly:
 * PolygonOffset written as 0.0 everywhere reachable). Its ground decals
 * (selection circles, blob shadows) re-draw receiver geometry with
 * additive blending, depth writes OFF and bias 0 under LESSEQUAL,
 * relying on cross-pass z invariance that a translation layer cannot
 * guarantee. The load-bearing fix (mirrored from mtld3d) is the
 * IMPLICIT DECAL BIAS: draws with depth-test on, z-write off, blending
 * on and zero app bias get setDepthBias(-1e-4 * 2^23, slope -1.5).
 *
 * Six bands, fixed-function XYZRHW quads, no shader blobs:
 *   A  coarse explicit bias   z=0.5  raw -5e-4 (eff -2.5e-4) vs sep 2e-6
 *      + blue z=0.6 sanity layer                            -> RED
 *   B  marginal explicit bias z=0.9  raw -3e-5 (eff -1.5e-5) vs sep 1e-5
 *      (1.5x margin inside the [0.5,1) octave)              -> RED
 *   C  slope-scale pass-through on sloped quads (unitless)  -> RED
 *   D  marginal explicit bias z=0.35 raw -3e-5 (eff -7.5e-6) vs sep 5e-6
 *      (octave-aware margin: documents the r = 2^(e-23) law) -> RED
 *   E  implicit decal rescue: base z=0.9, then blend-on/zwrite-off/bias-0
 *      "decal" at z+2e-5 (models the cross-pass ULP mismatch). The
 *      injected -1e-4 (eff -5e-5) must pull it in front       -> RED
 *   F  no-overfire guard: same as E but z-write ON — the heuristic must
 *      NOT fire, the decal must lose                          -> GREEN
 *
 * Probes at each band's center; the full tuple must match for PASS.
 * Results in depthbias_out.txt; PASS/FAIL on stdout. Self-exits. */
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
  const UINT W = 256, H = 516; /* 6 x 86-pixel bands */
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
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO));

  IDirect3DSurface9 *sysmem = NULL;
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));

  const DWORD biasZero = floatBits(0.0f);
  const DWORD biasCoarse = floatBits(-0.0005f);   /* A: 125x margin */
  const DWORD biasMarginal = floatBits(-3e-5f);   /* B/D: 1.5x octave-aware */
  const DWORD slopeHalfNeg = floatBits(-0.5f);    /* C */

  struct Vertex v[6];
  int frames = 0, verified = 0, ok = 0;
  DWORD prev[6] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                   0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
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

    /* ---- band A: coarse explicit bias trio ---- */
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

    /* ---- band B: marginal explicit bias, z=0.9 (eff raw/2) ---- */
    quad(v, 0.0f, bandH, (float)W, 2.0f * bandH, 0.9f, 0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasMarginal));
    quad(v, 0.0f, bandH, (float)W, 2.0f * bandH, 0.90001f, 0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZero));

    /* ---- band C: slope-scale pass-through ---- */
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

    /* ---- band D: marginal explicit bias, z=0.35 (eff raw/4) ---- */
    quad(v, 0.0f, 3.0f * bandH, (float)W, 4.0f * bandH, 0.35f, 0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasMarginal));
    quad(v, 0.0f, 3.0f * bandH, (float)W, 4.0f * bandH, 0.350005f, 0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZero));

    /* ---- band E: implicit decal rescue (the WoW circle pattern) ---- */
    quad(v, 0.0f, 4.0f * bandH, (float)W, 5.0f * bandH, 0.9f, 0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE));
    quad(v, 0.0f, 4.0f * bandH, (float)W, 5.0f * bandH, 0.90002f, 0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE));

    /* ---- band F: guard — z-write ON must suppress the injection ---- */
    quad(v, 0.0f, 5.0f * bandH, (float)W, 6.0f * bandH, 0.9f, 0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE));
    quad(v, 0.0f, 5.0f * bandH, (float)W, 6.0f * bandH, 0.90002f, 0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE));

    CHECK(IDirect3DDevice9_EndScene(dev));

    /* Poll the readback every frame: async pipeline compilation skips
     * draws until the PSO is ready, so early frames legitimately show
     * only the clear color. Verify once SOMETHING drew; give up after
     * ~600 frames. */
    if (++frames >= 3 && !verified) {
      IDirect3DSurface9 *bb = NULL;
      CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                                           D3DBACKBUFFER_TYPE_MONO, &bb));
      CHECK(IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem));
      IDirect3DSurface9_Release(bb);

      D3DLOCKED_RECT lr;
      CHECK(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY));
      const UINT probeY[6] = {43, 129, 215, 301, 387, 473};
      DWORD rgb[6];
      int i;
      for (i = 0; i < 6; i++)
        rgb[i] = (*(const DWORD *)((const BYTE *)lr.pBits +
                                   probeY[i] * lr.Pitch + (W / 2) * 4)) &
                 0x00FFFFFF;
      IDirect3DSurface9_UnlockRect(sysmem);

      /* Latch after TWO consecutive frames agree on the full tuple (the
       * async PSO compile can flip hot mid-frame; the next frame has all
       * draws live). */
      int anyDrawn = 0, stable = 1;
      for (i = 0; i < 6; i++) {
        anyDrawn |= rgb[i] != 0x00101010;
        stable &= rgb[i] == prev[i];
      }
      if (anyDrawn && stable) {
        verified = 1;
        LOG("probes: A=0x%06lx B=0x%06lx C=0x%06lx D=0x%06lx E=0x%06lx "
            "F=0x%06lx (frame %d)",
            (unsigned long)rgb[0], (unsigned long)rgb[1],
            (unsigned long)rgb[2], (unsigned long)rgb[3],
            (unsigned long)rgb[4], (unsigned long)rgb[5], frames);
        const int pass[6] = {
          rgb[0] == 0x00FF0000, rgb[1] == 0x00FF0000, rgb[2] == 0x00FF0000,
          rgb[3] == 0x00FF0000, rgb[4] == 0x00FF0000,
          rgb[5] == 0x0000FF00, /* F expects GREEN: injection suppressed */
        };
        LOG("%s: band A coarse explicit bias", pass[0] ? "ok" : "FAIL");
        LOG("%s: band B marginal explicit bias z=0.9 (eff raw/2)",
            pass[1] ? "ok" : "FAIL");
        LOG("%s: band C slope-scale pass-through", pass[2] ? "ok" : "FAIL");
        LOG("%s: band D marginal explicit bias z=0.35 (eff raw/4)",
            pass[3] ? "ok" : "FAIL");
        LOG("%s: band E implicit decal bias rescues the circle pattern",
            pass[4] ? "ok" : "FAIL");
        LOG("%s: band F z-write on suppresses the implicit bias",
            pass[5] ? "ok" : "FAIL");
        if (pass[0] && pass[1] && pass[2] && pass[3] && pass[4] && pass[5]) {
          LOG("PASS: depth bias semantics + implicit decal bias correct");
          ok = 1;
        } else {
          LOG("FAIL: depth bias semantics diverge (see bands above)");
        }
      } else if (frames >= 600) {
        verified = 1;
        LOG("FAIL: no stable frame in %d frames (pipeline never ready?)",
            frames);
      } else {
        for (i = 0; i < 6; i++)
          prev[i] = rgb[i];
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
