/* bench: driver-overhead microbenchmark for WoW-era workloads — thousands
 * of small draws per frame with rotating textures, constants, and blend
 * toggles. Measures wall-clock frame time (avg/median/p99 over the steady
 * window) and app-thread submit time per frame, so CS-ring backpressure
 * shows up even when presents are vsync-locked.
 *
 * Env knobs (all optional):
 *   BENCH_DRAWS   draws (or batches) per frame       (default 4000)
 *   BENCH_FRAMES  measured frames                    (default 300)
 *   BENCH_WARMUP  warmup frames, not counted         (default 120 — lets the
 *                 async PSO compile + buffer pools settle)
 *   BENCH_MODE    "up"    DrawPrimitiveUP quads (default; stresses the
 *                         suballocator)
 *                 "vb"    indexed static VB draws
 *                 "part"  particle systems: dynamic-VB NOOVERWRITE/DISCARD
 *                         lock+fill per batch, alpha-blended, z-write off
 *                 "rt"    shadow-style render-target round-trips: per unit,
 *                         switch to a small RT, clear, draw, switch back,
 *                         draw main-pass quad sampling that RT
 *                 "xform" FFP model draws: SetTransform(WORLD) + lighting
 *                         per draw from a static VB (per-draw FF constant
 *                         upload, the WoW M2/FFP pattern)
 *   BENCH_TEX     distinct textures rotated through  (default 8)
 *   BENCH_BATCH   part: quads per batch              (default 16)
 *   BENCH_RT      rt: render-target round-trips/frame (default 64;
 *                 BENCH_DRAWS is ignored in rt mode)
 *   BENCH_RTDRAWS rt: quads drawn inside each small RT pass (default 4)
 *
 * Results append to bench_out.txt as a single parseable line:
 *   RESULT mode=… draws=… frames=… avg_ms=… med_ms=… p99_ms=…
 *          submit_avg_ms=… fps=…
 * Self-exits. FFP only: no shader blobs needed. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Vertex {
  float x, y, z, rhw;
  DWORD color;
  float u, v;
};
#define FVF_BENCH (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct VertexXF {
  float x, y, z;
  float nx, ny, nz;
  float u, v;
};
#define FVF_XFORM (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

static int cmp_double(const void *a, const void *b) {
  double d = *(const double *)a - *(const double *)b;
  return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static int envi(const char *name, int def) {
  const char *v = getenv(name);
  return v && *v ? atoi(v) : def;
}

enum Mode { MODE_UP, MODE_VB, MODE_PART, MODE_RT, MODE_XFORM };

int main(void) {
  const UINT W = 800, H = 600;
  const int draws = envi("BENCH_DRAWS", 4000);
  const int frames = envi("BENCH_FRAMES", 300);
  const int warmup = envi("BENCH_WARMUP", 120);
  const int ntex = envi("BENCH_TEX", 8);
  const int batch = envi("BENCH_BATCH", 16);
  const int nrt = envi("BENCH_RT", 64);
  const int rtdraws = envi("BENCH_RTDRAWS", 4);
  const char *modestr = getenv("BENCH_MODE");
  enum Mode mode = MODE_UP;
  if (modestr) {
    if (!strcmp(modestr, "vb")) mode = MODE_VB;
    else if (!strcmp(modestr, "part")) mode = MODE_PART;
    else if (!strcmp(modestr, "rt")) mode = MODE_RT;
    else if (!strcmp(modestr, "xform")) mode = MODE_XFORM;
  }
  const char *modename[] = {"up", "vb", "part", "rt", "xform"};

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "bench";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("bench", "d9mt bench", WS_OVERLAPPEDWINDOW, 64,
                            64, W, H, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return 1;

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = W;
  pp.BackBufferHeight = H;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9 *dev = NULL;
  if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                     hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &pp, &dev)) ||
      !dev)
    return 1;

  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);

  /* small checkerboard textures, distinct colors */
  IDirect3DTexture9 **tex =
      (IDirect3DTexture9 **)calloc(ntex, sizeof(*tex));
  for (int t = 0; t < ntex; t++) {
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 32, 32, 1, 0,
                                              D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &tex[t], NULL)))
      return 1;
    D3DLOCKED_RECT lr;
    IDirect3DTexture9_LockRect(tex[t], 0, &lr, NULL, 0);
    for (int y = 0; y < 32; y++) {
      DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
      for (int x = 0; x < 32; x++)
        row[x] = ((x ^ y) & 4) ? 0xFF000000 | (0x123457u * (t + 1))
                               : 0xFFFFFFFF;
    }
    IDirect3DTexture9_UnlockRect(tex[t], 0);
  }

  /* optional static VB/IB with one small quad (vb mode) */
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DIndexBuffer9 *ib = NULL;
  if (mode == MODE_VB) {
    struct Vertex quad[4] = {
        {0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0},
        {8, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
        {0, 8, 0.5f, 1, 0xFFFFFFFF, 0, 1},
        {8, 8, 0.5f, 1, 0xFFFFFFFF, 1, 1},
    };
    WORD idx[6] = {0, 1, 2, 2, 1, 3};
    void *p;
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(quad), 0, FVF_BENCH,
                                        D3DPOOL_MANAGED, &vb, NULL);
    IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(quad), &p, 0);
    memcpy(p, quad, sizeof(quad));
    IDirect3DVertexBuffer9_Unlock(vb);
    IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(idx), 0, D3DFMT_INDEX16,
                                       D3DPOOL_MANAGED, &ib, NULL);
    IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(idx), &p, 0);
    memcpy(p, idx, sizeof(idx));
    IDirect3DIndexBuffer9_Unlock(ib);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex));
    IDirect3DDevice9_SetIndices(dev, ib);
  }

  /* particle mode: one dynamic ring VB + a static quad-pattern IB */
  IDirect3DVertexBuffer9 *dynvb = NULL;
  IDirect3DIndexBuffer9 *partib = NULL;
  UINT dynvb_size = 0, dynvb_pos = 0;
  const int maxbatch = 256;
  if (mode == MODE_PART) {
    dynvb_size = 256 * 1024;
    if (FAILED(IDirect3DDevice9_CreateVertexBuffer(
            dev, dynvb_size, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF_BENCH,
            D3DPOOL_DEFAULT, &dynvb, NULL)))
      return 1;
    /* index pattern for maxbatch quads: 4 verts / 6 indices each */
    WORD *ip;
    if (FAILED(IDirect3DDevice9_CreateIndexBuffer(
            dev, maxbatch * 6 * sizeof(WORD), 0, D3DFMT_INDEX16,
            D3DPOOL_MANAGED, &partib, NULL)))
      return 1;
    IDirect3DIndexBuffer9_Lock(partib, 0, 0, (void **)&ip, 0);
    for (int qd = 0; qd < maxbatch; qd++) {
      ip[qd * 6 + 0] = (WORD)(qd * 4 + 0);
      ip[qd * 6 + 1] = (WORD)(qd * 4 + 1);
      ip[qd * 6 + 2] = (WORD)(qd * 4 + 2);
      ip[qd * 6 + 3] = (WORD)(qd * 4 + 2);
      ip[qd * 6 + 4] = (WORD)(qd * 4 + 1);
      ip[qd * 6 + 5] = (WORD)(qd * 4 + 3);
    }
    IDirect3DIndexBuffer9_Unlock(partib);
    IDirect3DDevice9_SetIndices(dev, partib);
    IDirect3DDevice9_SetStreamSource(dev, 0, dynvb, 0, sizeof(struct Vertex));
    /* particle state: additive-ish alpha blend, no z-write */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
  }

  /* rt mode: small render targets + shared small depth surface */
  const int nrttex = 8;
  IDirect3DTexture9 *rttex[8] = {0};
  IDirect3DSurface9 *rtsurf[8] = {0};
  IDirect3DSurface9 *smallds = NULL, *backsurf = NULL, *mainds = NULL;
  if (mode == MODE_RT) {
    for (int i = 0; i < nrttex; i++) {
      if (FAILED(IDirect3DDevice9_CreateTexture(
              dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
              D3DPOOL_DEFAULT, &rttex[i], NULL)))
        return 1;
      IDirect3DTexture9_GetSurfaceLevel(rttex[i], 0, &rtsurf[i]);
    }
    if (FAILED(IDirect3DDevice9_CreateDepthStencilSurface(
            dev, 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
            &smallds, NULL)))
      return 1;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &backsurf);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &mainds);
  }

  /* xform mode: static world-space quad VB, lighting + per-draw transforms */
  IDirect3DVertexBuffer9 *xfvb = NULL;
  if (mode == MODE_XFORM) {
    struct VertexXF quad[6] = {
        {0, 0, 0.5f, 0, 0, -1, 0, 0}, {8, 0, 0.5f, 0, 0, -1, 1, 0},
        {0, 8, 0.5f, 0, 0, -1, 0, 1}, {0, 8, 0.5f, 0, 0, -1, 0, 1},
        {8, 0, 0.5f, 0, 0, -1, 1, 0}, {8, 8, 0.5f, 0, 0, -1, 1, 1},
    };
    void *p;
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(quad), 0, FVF_XFORM,
                                        D3DPOOL_MANAGED, &xfvb, NULL);
    IDirect3DVertexBuffer9_Lock(xfvb, 0, sizeof(quad), &p, 0);
    memcpy(p, quad, sizeof(quad));
    IDirect3DVertexBuffer9_Unlock(xfvb);
    IDirect3DDevice9_SetStreamSource(dev, 0, xfvb, 0, sizeof(struct VertexXF));

    /* ortho projection mapping x∈[0,W], y∈[0,H] to clip space */
    D3DMATRIX proj = {{{0}}};
    proj.m[0][0] = 2.0f / (float)W;
    proj.m[1][1] = -2.0f / (float)H;
    proj.m[2][2] = 1.0f;
    proj.m[3][0] = -1.0f;
    proj.m[3][1] = 1.0f;
    proj.m[3][3] = 1.0f;
    D3DMATRIX ident = {{{0}}};
    ident.m[0][0] = ident.m[1][1] = ident.m[2][2] = ident.m[3][3] = 1.0f;
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &proj);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &ident);

    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_AMBIENT,
                                    D3DCOLOR_XRGB(48, 48, 48));
    D3DLIGHT9 light = {0};
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
    light.Direction.z = 1.0f;
    IDirect3DDevice9_SetLight(dev, 0, &light);
    IDirect3DDevice9_LightEnable(dev, 0, TRUE);
    D3DMATERIAL9 mtl = {0};
    mtl.Diffuse.r = mtl.Diffuse.g = mtl.Diffuse.b = mtl.Diffuse.a = 1.0f;
    mtl.Ambient = mtl.Diffuse;
    IDirect3DDevice9_SetMaterial(dev, &mtl);
  }

  LARGE_INTEGER freq, t0, t1, s0, s1;
  QueryPerformanceFrequency(&freq);
  double *frame_ms = (double *)calloc(frames, sizeof(double));
  double submit_total = 0.0;
  int measured = 0;
  const int iters = (mode == MODE_RT) ? nrt : draws;
  const int nbatch = (batch < 1) ? 1 : (batch > maxbatch ? maxbatch : batch);

  MSG msg;
  for (int f = 0; f < warmup + frames; f++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    QueryPerformanceCounter(&t0);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(8, 8, 16), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, mode == MODE_XFORM ? FVF_XFORM : FVF_BENCH);

    QueryPerformanceCounter(&s0);
    switch (mode) {
    case MODE_UP:
    case MODE_VB:
      for (int d = 0; d < iters; d++) {
        /* WoW-ish churn: texture changes often, blend toggles occasionally */
        IDirect3DDevice9_SetTexture(dev, 0,
                                    (IDirect3DBaseTexture9 *)tex[d % ntex]);
        if ((d & 63) == 0)
          IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                          (d >> 6) & 1);
        const float x = (float)((d * 37) % (W - 8));
        const float y = (float)((d * 91) % (H - 8));
        if (mode == MODE_VB) {
          IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0,
                                                4, 0, 2);
        } else {
          const DWORD c = 0xFF000000 | (d * 2654435761u >> 8);
          const float z = 0.5f;
          struct Vertex v[6] = {
              {x, y, z, 1, c, 0, 0},         {x + 8, y, z, 1, c, 1, 0},
              {x, y + 8, z, 1, c, 0, 1},     {x, y + 8, z, 1, c, 0, 1},
              {x + 8, y, z, 1, c, 1, 0},     {x + 8, y + 8, z, 1, c, 1, 1},
          };
          IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex));
        }
      }
      break;

    case MODE_PART:
      for (int d = 0; d < iters; d++) {
        const UINT bytes = (UINT)(nbatch * 4) * sizeof(struct Vertex);
        DWORD flags = D3DLOCK_NOOVERWRITE;
        if (dynvb_pos + bytes > dynvb_size) {
          flags = D3DLOCK_DISCARD;
          dynvb_pos = 0;
        }
        void *p = NULL;
        if (FAILED(IDirect3DVertexBuffer9_Lock(dynvb, dynvb_pos, bytes, &p,
                                               flags)))
          break;
        struct Vertex *pv = (struct Vertex *)p;
        for (int qd = 0; qd < nbatch; qd++) {
          const float x = (float)(((d * 131 + qd * 17) * 37) % (W - 6));
          const float y = (float)(((d * 79 + qd * 29) * 91) % (H - 6));
          const DWORD c = 0x80FFFFFF; /* half-alpha white */
          pv[qd * 4 + 0] = (struct Vertex){x, y, 0.5f, 1, c, 0, 0};
          pv[qd * 4 + 1] = (struct Vertex){x + 6, y, 0.5f, 1, c, 1, 0};
          pv[qd * 4 + 2] = (struct Vertex){x, y + 6, 0.5f, 1, c, 0, 1};
          pv[qd * 4 + 3] = (struct Vertex){x + 6, y + 6, 0.5f, 1, c, 1, 1};
        }
        IDirect3DVertexBuffer9_Unlock(dynvb);
        IDirect3DDevice9_SetTexture(dev, 0,
                                    (IDirect3DBaseTexture9 *)tex[d % 4]);
        IDirect3DDevice9_DrawIndexedPrimitive(
            dev, D3DPT_TRIANGLELIST,
            (INT)(dynvb_pos / sizeof(struct Vertex)), 0, (UINT)(nbatch * 4),
            0, (UINT)(nbatch * 2));
        dynvb_pos += bytes;
      }
      break;

    case MODE_RT:
      for (int u = 0; u < iters; u++) {
        /* shadow-style round trip: render into a small RT… */
        IDirect3DDevice9_SetRenderTarget(dev, 0, rtsurf[u % nrttex]);
        IDirect3DDevice9_SetDepthStencilSurface(dev, smallds);
        IDirect3DDevice9_Clear(dev, 0, NULL,
                               D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f,
                               0);
        for (int d = 0; d < rtdraws; d++) {
          IDirect3DDevice9_SetTexture(
              dev, 0, (IDirect3DBaseTexture9 *)tex[(u + d) % ntex]);
          const float x = (float)((d * 13) % 56);
          const float y = (float)((d * 29) % 56);
          const DWORD c = 0xFFFFFFFF;
          struct Vertex v[6] = {
              {x, y, 0.5f, 1, c, 0, 0},         {x + 8, y, 0.5f, 1, c, 1, 0},
              {x, y + 8, 0.5f, 1, c, 0, 1},     {x, y + 8, 0.5f, 1, c, 0, 1},
              {x + 8, y, 0.5f, 1, c, 1, 0},     {x + 8, y + 8, 0.5f, 1, c, 1, 1},
          };
          IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex));
        }
        /* …then back to the main pass, sampling what we just rendered */
        IDirect3DDevice9_SetRenderTarget(dev, 0, backsurf);
        IDirect3DDevice9_SetDepthStencilSurface(dev, mainds);
        IDirect3DDevice9_SetTexture(dev, 0,
                                    (IDirect3DBaseTexture9 *)rttex[u % nrttex]);
        const float x = (float)((u * 37) % (W - 16));
        const float y = (float)((u * 91) % (H - 16));
        const DWORD c = 0xFFFFFFFF;
        struct Vertex v[6] = {
            {x, y, 0.5f, 1, c, 0, 0},           {x + 16, y, 0.5f, 1, c, 1, 0},
            {x, y + 16, 0.5f, 1, c, 0, 1},      {x, y + 16, 0.5f, 1, c, 0, 1},
            {x + 16, y, 0.5f, 1, c, 1, 0},      {x + 16, y + 16, 0.5f, 1, c, 1, 1},
        };
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                         sizeof(struct Vertex));
      }
      break;

    case MODE_XFORM:
      for (int d = 0; d < iters; d++) {
        D3DMATRIX world = {{{0}}};
        world.m[0][0] = world.m[1][1] = world.m[2][2] = world.m[3][3] = 1.0f;
        world.m[3][0] = (float)((d * 37) % (W - 8));
        world.m[3][1] = (float)((d * 91) % (H - 8));
        IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &world);
        IDirect3DDevice9_SetTexture(dev, 0,
                                    (IDirect3DBaseTexture9 *)tex[d % ntex]);
        IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
      }
      break;
    }
    QueryPerformanceCounter(&s1);

    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    QueryPerformanceCounter(&t1);

    if (f >= warmup) {
      frame_ms[measured] =
          (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
      submit_total +=
          (double)(s1.QuadPart - s0.QuadPart) * 1000.0 / (double)freq.QuadPart;
      measured++;
    }
  }

done:;
  FILE *out = fopen("bench_out.txt", "a");
  if (out && measured > 0) {
    double sum = 0;
    for (int i = 0; i < measured; i++)
      sum += frame_ms[i];
    qsort(frame_ms, measured, sizeof(double), cmp_double);
    const double avg = sum / measured;
    const double med = frame_ms[measured / 2];
    const double p99 = frame_ms[(int)(measured * 0.99) < measured
                                    ? (int)(measured * 0.99)
                                    : measured - 1];
    fprintf(out,
            "RESULT mode=%s draws=%d frames=%d avg_ms=%.3f med_ms=%.3f "
            "p99_ms=%.3f submit_avg_ms=%.3f fps=%.1f\n",
            modename[mode], iters, measured, avg, med, p99,
            submit_total / measured, 1000.0 / avg);
    fclose(out);
  }
  return 0;
}
