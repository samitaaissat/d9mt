/* rtdefer.c — content validation for the shadow-style render-target
 * round-trip pattern that D9MT_PASS_DEFER reorders (bench.c BENCH_MODE=rt
 * only measures time; this test proves the pixels).
 *
 * Per trip u (24 trips = 3 cycles over an 8-slot RT pool, so slots are
 * REUSED with new colors — the round-robin W∩S hazard):
 *   - SetRenderTarget(rt[u%8]) + small DS, Clear to border color B(u),
 *     draw an untextured quad of color Q(u) over the RT's center
 *   - back to the backbuffer + main DS, draw a 16x16 quad at a per-trip
 *     position sampling rt[u%8]
 * At frame end (before Present) the backbuffer is read back and every
 * trip's quad is probed at its center (expects Q(u), the RT center) and
 * near its corner (expects B(u), the RT border). A pass that encodes in
 * the wrong order shows a LATER cycle's colors in an EARLIER trip's quad.
 * The readback itself exercises the mandatory flush-at-blit path.
 *
 * Run with D9MT_ASYNC=0: async PSO compilation drops draws until the PSO is
 * ready, and unlike depthbias this test has no wait-until-stable loop. The
 * first frames are still warmup; the last frames are validated, which also
 * covers suspension across a Present. Prints PASS/FAIL to rtdefer_out.txt
 * (harness marker convention).
 *
 *   tools/bench-wowsilicon.sh run rtdefer.exe D9MT_ASYNC=0 [D9MT_PASS_DEFER=1]
 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define FVF_T (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };

static FILE *g_out;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(g_out, "FAIL: " __VA_ARGS__); fprintf(g_out, "\n"); \
                   fflush(g_out); return 1; } \
  } while (0)

static const int W = 640, H = 480;
static const int NTRIPS = 24, NRT = 8;

static void quad(struct Vertex *v, float x0, float y0, float x1, float y1,
                 DWORD c) {
  /* two triangles, full 0..1 UVs */
  v[0] = (struct Vertex){x0, y0, 0.5f, 1, c, 0, 0};
  v[1] = (struct Vertex){x1, y0, 0.5f, 1, c, 1, 0};
  v[2] = (struct Vertex){x0, y1, 0.5f, 1, c, 0, 1};
  v[3] = (struct Vertex){x0, y1, 0.5f, 1, c, 0, 1};
  v[4] = (struct Vertex){x1, y0, 0.5f, 1, c, 1, 0};
  v[5] = (struct Vertex){x1, y1, 0.5f, 1, c, 1, 1};
}

/* distinct per-trip colors; cycle 3 reuses slots with different values */
static DWORD borderColor(int u) { return 0xFF000000 | ((16 + u * 9) << 16); }
static DWORD centerColor(int u) { return 0xFF000000 | ((16 + u * 9) << 8); }

static int colorNear(DWORD a, DWORD b) {
  int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
  int dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
  int db = (int)(a & 0xFF) - (int)(b & 0xFF);
  if (dr < 0) dr = -dr; if (dg < 0) dg = -dg; if (db < 0) db = -db;
  return dr <= 2 && dg <= 2 && db <= 2;
}

static int quadX(int u) { return 8 + (u % 12) * 48; }
static int quadY(int u) { return 8 + (u / 12) * 48; }

int main(void) {
  g_out = fopen("rtdefer_out.txt", "w");
  if (!g_out) return 1;

  HWND hwnd = CreateWindowA("STATIC", "rtdefer", WS_POPUP, 0, 0, W, H,
                            NULL, NULL, NULL, NULL);
  CHECK(hwnd != NULL, "CreateWindow");

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  CHECK(d3d != NULL, "Direct3DCreate9");

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = W;
  pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;

  IDirect3DDevice9 *dev = NULL;
  CHECK(SUCCEEDED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
            &dev)), "CreateDevice");

  IDirect3DTexture9 *rttex[8] = {0};
  IDirect3DSurface9 *rtsurf[8] = {0};
  for (int i = 0; i < NRT; i++) {
    CHECK(SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 64, 64, 1,
              D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
              &rttex[i], NULL)), "CreateTexture rt%d", i);
    IDirect3DTexture9_GetSurfaceLevel(rttex[i], 0, &rtsurf[i]);
  }
  IDirect3DSurface9 *smallds = NULL, *backsurf = NULL, *mainds = NULL;
  CHECK(SUCCEEDED(IDirect3DDevice9_CreateDepthStencilSurface(dev, 64, 64,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &smallds, NULL)),
        "CreateDepthStencilSurface");
  IDirect3DDevice9_GetRenderTarget(dev, 0, &backsurf);
  IDirect3DDevice9_GetDepthStencilSurface(dev, &mainds);

  IDirect3DSurface9 *sysmem = NULL;
  CHECK(SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H,
            D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL)),
        "CreateOffscreenPlainSurface");

  /* exact color path: point sampling, clamp, no blending/lighting/dither */
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_DITHERENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  /* alpha stage must stay defined while no texture is bound */
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,
                                        D3DTOP_SELECTARG1);
  IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1,
                                        D3DTA_DIFFUSE);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  IDirect3DDevice9_SetFVF(dev, FVF_T);

  struct Vertex v[6];

  const int warmup = 5, total = 8; /* validate the last 3 frames */
  for (int frame = 0; frame < total; frame++) {
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(8, 8, 16), 1.0f, 0);
    CHECK(SUCCEEDED(IDirect3DDevice9_BeginScene(dev)), "BeginScene f%d", frame);

    for (int u = 0; u < NTRIPS; u++) {
      /* --- interlude: render into the small RT --- */
      IDirect3DDevice9_SetRenderTarget(dev, 0, rtsurf[u % NRT]);
      IDirect3DDevice9_SetDepthStencilSurface(dev, smallds);
      IDirect3DDevice9_Clear(dev, 0, NULL,
                             D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                             borderColor(u), 1.0f, 0);
      /* untextured center quad: diffuse only */
      IDirect3DDevice9_SetTexture(dev, 0, NULL);
      IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP,
                                            D3DTOP_SELECTARG1);
      IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1,
                                            D3DTA_DIFFUSE);
      quad(v, 16, 16, 48, 48, centerColor(u));
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                       sizeof(struct Vertex));

      /* --- back to the main pass: quad sampling what we just rendered --- */
      IDirect3DDevice9_SetRenderTarget(dev, 0, backsurf);
      IDirect3DDevice9_SetDepthStencilSurface(dev, mainds);
      IDirect3DDevice9_SetTexture(dev, 0,
                                  (IDirect3DBaseTexture9 *)rttex[u % NRT]);
      IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1,
                                            D3DTA_TEXTURE);
      float x = (float)quadX(u), y = (float)quadY(u);
      quad(v, x, y, x + 16, y + 16, 0xFFFFFFFF);
      IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                       sizeof(struct Vertex));
    }

    CHECK(SUCCEEDED(IDirect3DDevice9_EndScene(dev)), "EndScene f%d", frame);

    if (frame < warmup) {
      IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
      continue;
    }

    /* readback BEFORE present: mandatory-flush path + content check */
    CHECK(SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, backsurf,
              sysmem)), "GetRenderTargetData f%d", frame);

    D3DLOCKED_RECT lr;
    CHECK(SUCCEEDED(IDirect3DSurface9_LockRect(sysmem, &lr, NULL,
              D3DLOCK_READONLY)), "LockRect f%d", frame);
    for (int u = 0; u < NTRIPS; u++) {
      const DWORD *row;
      int px, py;
      /* quad center -> RT texel (32,32): the drawn center color */
      px = quadX(u) + 8; py = quadY(u) + 8;
      row = (const DWORD *)((const BYTE *)lr.pBits + py * lr.Pitch);
      DWORD got = row[px] | 0xFF000000;
      if (!colorNear(got, centerColor(u))) {
        fprintf(g_out, "FAIL: f%d trip %d center: got %08lX want %08lX\n",
                frame, u, (unsigned long)got,
                (unsigned long)centerColor(u));
        fflush(g_out);
        IDirect3DSurface9_UnlockRect(sysmem);
        return 1;
      }
      /* quad corner (+1,+1) -> RT texel (4,4): the clear/border color */
      px = quadX(u) + 1; py = quadY(u) + 1;
      row = (const DWORD *)((const BYTE *)lr.pBits + py * lr.Pitch);
      got = row[px] | 0xFF000000;
      if (!colorNear(got, borderColor(u))) {
        fprintf(g_out, "FAIL: f%d trip %d border: got %08lX want %08lX\n",
                frame, u, (unsigned long)got,
                (unsigned long)borderColor(u));
        fflush(g_out);
        IDirect3DSurface9_UnlockRect(sysmem);
        return 1;
      }
    }
    IDirect3DSurface9_UnlockRect(sysmem);

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    fprintf(g_out, "ok: frame %d — all %d trips render the right colors\n",
            frame, NTRIPS);
  }

  fprintf(g_out, "PASS: RT round-trip content correct across %d trips x %d "
                 "frames (8-slot reuse included)\n", NTRIPS, total - warmup);
  fflush(g_out);
  return 0;
}
