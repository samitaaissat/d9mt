/* bench: driver-overhead microbenchmark for WoW-era workloads — thousands
 * of small draws per frame with rotating textures, constants, and blend
 * toggles. Measures wall-clock frame time (avg/median/p99 over the steady
 * window) and app-thread submit time per frame, so CS-ring backpressure
 * shows up even when presents are vsync-locked.
 *
 * Env knobs (all optional):
 *   BENCH_DRAWS   draws per frame            (default 4000)
 *   BENCH_FRAMES  measured frames            (default 300)
 *   BENCH_WARMUP  warmup frames, not counted (default 120 — lets the async
 *                 PSO compile + buffer pools settle)
 *   BENCH_MODE    "up" DrawPrimitiveUP quads (default; stresses the
 *                 suballocator) | "vb" indexed static VB draws
 *   BENCH_TEX     distinct textures rotated through (default 8)
 *
 * Results append to bench_out.txt as a single parseable line:
 *   RESULT mode=up draws=4000 frames=300 avg_ms=… med_ms=… p99_ms=…
 *          submit_avg_ms=… fps=…
 * Self-exits. Fixed-function only (XYZRHW + DIFFUSE + TEX1): no shader
 * blobs needed, mirrors WoW's FFP-heavy vanilla path. */
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

int main(void) {
  const UINT W = 800, H = 600;
  const int draws = envi("BENCH_DRAWS", 4000);
  const int frames = envi("BENCH_FRAMES", 300);
  const int warmup = envi("BENCH_WARMUP", 120);
  const int ntex = envi("BENCH_TEX", 8);
  const char *mode = getenv("BENCH_MODE");
  const int use_vb = mode && !strcmp(mode, "vb");

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

  /* optional static VB/IB with one small quad */
  IDirect3DVertexBuffer9 *vb = NULL;
  IDirect3DIndexBuffer9 *ib = NULL;
  if (use_vb) {
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

  LARGE_INTEGER freq, t0, t1, s0, s1;
  QueryPerformanceFrequency(&freq);
  double *frame_ms = (double *)calloc(frames, sizeof(double));
  double submit_total = 0.0;
  int measured = 0;

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
    IDirect3DDevice9_SetFVF(dev, FVF_BENCH);

    QueryPerformanceCounter(&s0);
    for (int d = 0; d < draws; d++) {
      /* WoW-ish churn: texture changes often, blend toggles occasionally */
      IDirect3DDevice9_SetTexture(dev, 0,
                                  (IDirect3DBaseTexture9 *)tex[d % ntex]);
      if ((d & 63) == 0)
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE,
                                        (d >> 6) & 1);
      const float x = (float)((d * 37) % (W - 8));
      const float y = (float)((d * 91) % (H - 8));
      if (use_vb) {
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
            use_vb ? "vb" : "up", draws, measured, avg, med, p99,
            submit_total / measured, 1000.0 / avg);
    fclose(out);
  }
  return 0;
}
