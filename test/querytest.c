/* querytest: proves GPU queries end-to-end on the d3d9fe Metal backend.
 *
 *  - OCCLUSION (precise): a 100x100 pretransformed quad is drawn twice with
 *    a mid-query Clear(ZBUFFER) between the draws, which forces a render-
 *    pass restart — the query must span the pass split and report exactly
 *    2 * 10000 samples.
 *  - A second occlusion query with no draws inside must report 0.
 *  - EVENT: must signal after the flush retires.
 *  - TIMESTAMP / TIMESTAMPDISJOINT / TIMESTAMPFREQ: timestamp must be
 *    non-zero (ns), the disjoint flag FALSE, the frequency 1e9.
 *
 * Results go to querytest_out.txt. Renders forever after PASS (the suite
 * runner kills the process).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static FILE *g_out;
#define LOG(...)                                                               \
  do {                                                                         \
    fprintf(g_out, __VA_ARGS__);                                               \
    fputc('\n', g_out);                                                        \
    fflush(g_out);                                                             \
  } while (0)
#define CHECK(expr)                                                            \
  do {                                                                         \
    HRESULT hr_ = (expr);                                                      \
    if (FAILED(hr_)) {                                                         \
      LOG("FAIL: %s -> 0x%08lx", #expr, (unsigned long)hr_);                   \
      return 1;                                                                \
    }                                                                          \
    LOG("ok: %s", #expr);                                                      \
  } while (0)

#define FVF_QUAD (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

struct Vertex {
  float x, y, z, rhw;
  DWORD color;
};

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

/* two triangles covering pixel rect [100,200)x[100,200) exactly: 10000 px */
static const struct Vertex quad[6] = {
    {100.0f, 100.0f, 0.5f, 1.0f, 0xFFFF8000},
    {200.0f, 100.0f, 0.5f, 1.0f, 0xFFFF8000},
    {100.0f, 200.0f, 0.5f, 1.0f, 0xFFFF8000},
    {200.0f, 100.0f, 0.5f, 1.0f, 0xFFFF8000},
    {200.0f, 200.0f, 0.5f, 1.0f, 0xFFFF8000},
    {100.0f, 200.0f, 0.5f, 1.0f, 0xFFFF8000},
};

/* polls q with FLUSH until D3D_OK or ~15s; returns hr */
static HRESULT poll_query(IDirect3DQuery9 *q, void *data, DWORD size,
                          const char *name) {
  HRESULT hr = S_FALSE;
  for (int i = 0; i < 1500; i++) {
    hr = IDirect3DQuery9_GetData(q, data, size, D3DGETDATA_FLUSH);
    if (hr != S_FALSE)
      break;
    Sleep(10);
  }
  LOG("poll %s -> 0x%08lx", name, (unsigned long)hr);
  return hr;
}

int main(void) {
  g_out = fopen("querytest_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "querytest";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("querytest", "d9mt querytest",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800,
                            600, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) {
    LOG("FAIL: CreateWindow");
    return 1;
  }

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    LOG("FAIL: Direct3DCreate9");
    return 1;
  }
  LOG("ok: Direct3DCreate9");

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 800;
  pp.BackBufferHeight = 600;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                &dev));

  IDirect3DQuery9 *q_occl = NULL, *q_zero = NULL, *q_event = NULL;
  IDirect3DQuery9 *q_ts = NULL, *q_tsd = NULL, *q_tsf = NULL;
  CHECK(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &q_occl));
  CHECK(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &q_zero));
  CHECK(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &q_event));
  CHECK(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMP, &q_ts));
  CHECK(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMPDISJOINT,
                                     &q_tsd));
  CHECK(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMPFREQ, &q_tsf));

  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));

  /* ---- the measured frame ---- */
  CHECK(IDirect3DDevice9_Clear(dev, 0, NULL,
                               D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0));
  CHECK(IDirect3DDevice9_BeginScene(dev));
  CHECK(IDirect3DDevice9_SetFVF(dev, FVF_QUAD));

  CHECK(IDirect3DQuery9_Issue(q_tsd, D3DISSUE_BEGIN));
  CHECK(IDirect3DQuery9_Issue(q_occl, D3DISSUE_BEGIN));

  CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad,
                                         sizeof(struct Vertex)));

  /* mid-query depth clear: becomes a deferred clear -> the next draw
   * restarts the render pass -> the query must span the split */
  CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0));

  CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad,
                                         sizeof(struct Vertex)));

  CHECK(IDirect3DQuery9_Issue(q_occl, D3DISSUE_END));

  /* empty occlusion scope: must come back 0 without hanging */
  CHECK(IDirect3DQuery9_Issue(q_zero, D3DISSUE_BEGIN));
  CHECK(IDirect3DQuery9_Issue(q_zero, D3DISSUE_END));

  CHECK(IDirect3DQuery9_Issue(q_ts, D3DISSUE_END));
  CHECK(IDirect3DQuery9_Issue(q_tsd, D3DISSUE_END));
  CHECK(IDirect3DQuery9_Issue(q_event, D3DISSUE_END));

  CHECK(IDirect3DDevice9_EndScene(dev));
  CHECK(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));

  /* ---- poll results ---- */
  int fail = 0;

  DWORD samples = 0xdeadbeef;
  if (poll_query(q_occl, &samples, sizeof(samples), "occlusion") != D3D_OK) {
    LOG("FAIL: occlusion GetData did not complete");
    fail = 1;
  } else {
    LOG("occlusion samples = %lu (expect 20000)", (unsigned long)samples);
    if (samples != 20000)
      fail = 1;
  }

  DWORD zsamples = 0xdeadbeef;
  if (poll_query(q_zero, &zsamples, sizeof(zsamples), "occlusion-empty")
      != D3D_OK) {
    LOG("FAIL: empty occlusion GetData did not complete");
    fail = 1;
  } else {
    LOG("empty occlusion samples = %lu (expect 0)", (unsigned long)zsamples);
    if (zsamples != 0)
      fail = 1;
  }

  BOOL signaled = 2;
  if (poll_query(q_event, &signaled, sizeof(signaled), "event") != D3D_OK) {
    LOG("FAIL: event GetData did not complete");
    fail = 1;
  } else {
    LOG("event signaled = %d (expect 1)", (int)signaled);
    if (signaled != TRUE)
      fail = 1;
  }

  UINT64 ts = 0;
  if (poll_query(q_ts, &ts, sizeof(ts), "timestamp") != D3D_OK) {
    LOG("FAIL: timestamp GetData did not complete");
    fail = 1;
  } else {
    LOG("timestamp = %llu ns (expect > 0)", (unsigned long long)ts);
    if (ts == 0)
      fail = 1;
  }

  BOOL disjoint = 2;
  if (poll_query(q_tsd, &disjoint, sizeof(disjoint), "tsdisjoint") != D3D_OK) {
    LOG("FAIL: tsdisjoint GetData did not complete");
    fail = 1;
  } else {
    LOG("timestamp disjoint = %d (expect 0)", (int)disjoint);
    if (disjoint != FALSE)
      fail = 1;
  }

  UINT64 freq = 0;
  if (poll_query(q_tsf, &freq, sizeof(freq), "tsfreq") != D3D_OK) {
    LOG("FAIL: tsfreq GetData did not complete");
    fail = 1;
  } else {
    LOG("timestamp freq = %llu Hz (expect 1000000000)",
        (unsigned long long)freq);
    if (freq != 1000000000ull)
      fail = 1;
  }

  /* re-use the occlusion query a second time (recycle path) */
  CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                               D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0));
  CHECK(IDirect3DDevice9_BeginScene(dev));
  CHECK(IDirect3DDevice9_SetFVF(dev, FVF_QUAD));
  CHECK(IDirect3DQuery9_Issue(q_occl, D3DISSUE_BEGIN));
  CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad,
                                         sizeof(struct Vertex)));
  CHECK(IDirect3DQuery9_Issue(q_occl, D3DISSUE_END));
  CHECK(IDirect3DDevice9_EndScene(dev));
  CHECK(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL));

  samples = 0xdeadbeef;
  if (poll_query(q_occl, &samples, sizeof(samples), "occlusion-reuse")
      != D3D_OK) {
    LOG("FAIL: reused occlusion GetData did not complete");
    fail = 1;
  } else {
    LOG("reused occlusion samples = %lu (expect 10000)",
        (unsigned long)samples);
    if (samples != 10000)
      fail = 1;
  }

  if (fail) {
    LOG("FAIL: query results incorrect");
    return 1;
  }
  LOG("PASS: occlusion (pass-split spanning) + event + timestamps");

  /* keep rendering; the suite runner kills the process */
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        return 0;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(16, 16, 64), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, FVF_QUAD);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, quad,
                                     sizeof(struct Vertex));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }
}
