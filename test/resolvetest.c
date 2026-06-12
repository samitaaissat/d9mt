/* resolvetest: proves MSAA resolves, including the depth-stencil resolve
 * (BACKEND-SURFACE 1.4 ResolveZ semantics; the GTA IV INTZ-style path).
 *
 *  1. caps sanity: 4x must be available; if 8x is REPORTED available the
 *     8x render-target creation must also succeed (no over-promising).
 *  2. render a red triangle at z=0.25 into a 4x MSAA RT + 4x D24S8.
 *  3. StretchRect color MSAA -> 1x (AVERAGE resolve), read back, verify
 *     center==red / corner==clear.
 *  4. StretchRect depth MSAA -> 1x D24S8 (resolveImage AVERAGE/SAMPLE_ZERO,
 *     our sample-pass depth+stencil resolve), then draw a fullscreen green
 *     quad at z=0.5 against the RESOLVED depth (ZFUNC LESSEQUAL, no z
 *     clear): center (resolved 0.25) must reject it, corner (resolved 1.0)
 *     must accept it. Read back and verify center==black / corner==green.
 *
 * Reuses the shadertri SM3 bytecode (pos+color). Results go to
 * resolvetest_out.txt; renders forever after PASS (the suite runner kills).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#include "shadertri_vs_bytecode.h"
#include "shadertri_ps_bytecode.h"

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

struct Vertex {
  float x, y, z;
  DWORD color;
};

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

static int px_eq(DWORD a, DWORD b) {
  int dr = (int)((a >> 16) & 0xff) - (int)((b >> 16) & 0xff);
  int dg = (int)((a >> 8) & 0xff) - (int)((b >> 8) & 0xff);
  int db = (int)(a & 0xff) - (int)(b & 0xff);
  if (dr < 0) dr = -dr;
  if (dg < 0) dg = -dg;
  if (db < 0) db = -db;
  return dr <= 2 && dg <= 2 && db <= 2;
}

/* reads back rt into sysmem and returns center + corner pixels */
static HRESULT readback(IDirect3DDevice9 *dev, IDirect3DSurface9 *rt,
                        IDirect3DSurface9 *sysmem, DWORD *center,
                        DWORD *corner) {
  HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sysmem);
  if (FAILED(hr))
    return hr;
  D3DLOCKED_RECT lr;
  hr = IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
  if (FAILED(hr))
    return hr;
  *center = *(const DWORD *)((const BYTE *)lr.pBits + 128 * lr.Pitch + 128 * 4);
  *corner = *(const DWORD *)((const BYTE *)lr.pBits + 8 * lr.Pitch + 8 * 4);
  return IDirect3DSurface9_UnlockRect(sysmem);
}

int main(void) {
  g_out = fopen("resolvetest_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "resolvetest";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("resolvetest", "d9mt resolvetest",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 256,
                            256, NULL, NULL, wc.hInstance, NULL);
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

  /* ---- caps: 4x required; 8x must not be over-promised ---- */
  DWORD q4 = 0, q8 = 0;
  HRESULT hr4 = IDirect3D9_CheckDeviceMultiSampleType(
      d3d, 0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
      D3DMULTISAMPLE_4_SAMPLES, &q4);
  HRESULT hr8 = IDirect3D9_CheckDeviceMultiSampleType(
      d3d, 0, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
      D3DMULTISAMPLE_8_SAMPLES, &q8);
  LOG("CheckDeviceMultiSampleType 4x=0x%08lx 8x=0x%08lx",
      (unsigned long)hr4, (unsigned long)hr8);
  if (FAILED(hr4)) {
    LOG("FAIL: 4x multisampling not available");
    return 1;
  }

  D3DPRESENT_PARAMETERS pp = {0};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferWidth = 256;
  pp.BackBufferHeight = 256;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                &dev));

  if (SUCCEEDED(hr8)) {
    /* reported available -> creation must work, else caps over-promise */
    IDirect3DSurface9 *rt8 = NULL;
    HRESULT hrc = IDirect3DDevice9_CreateRenderTarget(
        dev, 256, 256, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_8_SAMPLES, 0, FALSE,
        &rt8, NULL);
    if (FAILED(hrc)) {
      LOG("FAIL: 8x advertised by caps but CreateRenderTarget -> 0x%08lx",
          (unsigned long)hrc);
      return 1;
    }
    IDirect3DSurface9_Release(rt8);
    LOG("ok: 8x advertised and creatable");
  } else {
    LOG("ok: 8x not advertised (expected on M1-class GPUs)");
  }

  /* ---- resources ---- */
  IDirect3DSurface9 *rtMs = NULL, *dsMs = NULL, *ds1x = NULL, *rt1x = NULL,
                    *sysmem = NULL, *backbuf = NULL;
  CHECK(IDirect3DDevice9_CreateRenderTarget(dev, 256, 256, D3DFMT_A8R8G8B8,
                                            D3DMULTISAMPLE_4_SAMPLES, 0, FALSE,
                                            &rtMs, NULL));
  CHECK(IDirect3DDevice9_CreateDepthStencilSurface(
      dev, 256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &dsMs,
      NULL));
  CHECK(IDirect3DDevice9_CreateDepthStencilSurface(
      dev, 256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds1x,
      NULL));
  CHECK(IDirect3DDevice9_CreateRenderTarget(dev, 256, 256, D3DFMT_A8R8G8B8,
                                            D3DMULTISAMPLE_NONE, 0, FALSE,
                                            &rt1x, NULL));
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, 256, 256, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));
  CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                       &backbuf));

  /* ---- shaders / geometry ---- */
  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  CHECK(IDirect3DDevice9_CreateVertexShader(
      dev, (const DWORD *)shadertri_vs_bytecode, &vs));
  CHECK(IDirect3DDevice9_CreatePixelShader(
      dev, (const DWORD *)shadertri_ps_bytecode, &ps));

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
       0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,
       0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl));

  /* red center triangle at z=0.25, then a fullscreen green quad at z=0.5 */
  static const struct Vertex verts[9] = {
      {-0.8f, -0.8f, 0.25f, 0xFFFF0000},
      {0.0f, 0.8f, 0.25f, 0xFFFF0000},
      {0.8f, -0.8f, 0.25f, 0xFFFF0000},

      {-1.0f, -1.0f, 0.5f, 0xFF00FF00},
      {-1.0f, 1.0f, 0.5f, 0xFF00FF00},
      {1.0f, -1.0f, 0.5f, 0xFF00FF00},
      {1.0f, -1.0f, 0.5f, 0xFF00FF00},
      {-1.0f, 1.0f, 0.5f, 0xFF00FF00},
      {1.0f, 1.0f, 0.5f, 0xFF00FF00},
  };

  IDirect3DVertexBuffer9 *vb = NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                            D3DPOOL_DEFAULT, &vb, NULL));
  void *p = NULL;
  CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0));
  memcpy(p, verts, sizeof(verts));
  CHECK(IDirect3DVertexBuffer9_Unlock(vb));

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1};
  static const float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  CHECK(IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4));
  CHECK(IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, tint, 1));
  CHECK(IDirect3DDevice9_SetVertexDeclaration(dev, decl));
  CHECK(IDirect3DDevice9_SetVertexShader(dev, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(dev, ps));
  CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex)));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE));

  /* ---- pass 1: triangle into the 4x MSAA targets ---- */
  CHECK(IDirect3DDevice9_SetRenderTarget(dev, 0, rtMs));
  CHECK(IDirect3DDevice9_SetDepthStencilSurface(dev, dsMs));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL));
  CHECK(IDirect3DDevice9_Clear(dev, 0, NULL,
                               D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
                                   D3DCLEAR_STENCIL,
                               D3DCOLOR_XRGB(16, 24, 40), 1.0f, 0));
  CHECK(IDirect3DDevice9_BeginScene(dev));
  CHECK(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1));
  CHECK(IDirect3DDevice9_EndScene(dev));

  /* ---- color resolve: MSAA RT -> 1x RT ---- */
  CHECK(IDirect3DDevice9_StretchRect(dev, rtMs, NULL, rt1x, NULL,
                                     D3DTEXF_NONE));
  DWORD center = 0, corner = 0;
  CHECK(readback(dev, rt1x, sysmem, &center, &corner));
  LOG("color resolve: center=%08lx corner=%08lx", (unsigned long)center,
      (unsigned long)corner);
  if (!px_eq(center, 0xFFFF0000) || !px_eq(corner, 0xFF101828)) {
    LOG("FAIL: color resolve mismatch (want center FFFF0000 corner FF101828)");
    return 1;
  }
  LOG("ok: AVERAGE color resolve");

  /* ---- depth resolve: MSAA D24S8 -> 1x D24S8 ---- */
  CHECK(IDirect3DDevice9_StretchRect(dev, dsMs, NULL, ds1x, NULL,
                                     D3DTEXF_NONE));

  /* ---- pass 2: green quad at z=0.5 against the RESOLVED depth ---- */
  CHECK(IDirect3DDevice9_SetRenderTarget(dev, 0, rt1x));
  CHECK(IDirect3DDevice9_SetDepthStencilSurface(dev, ds1x));
  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE));
  /* clear COLOR only: the depth contents are the resolve result */
  CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                               D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0));
  CHECK(IDirect3DDevice9_BeginScene(dev));
  CHECK(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 3, 2));
  CHECK(IDirect3DDevice9_EndScene(dev));

  CHECK(readback(dev, rt1x, sysmem, &center, &corner));
  LOG("depth resolve probe: center=%08lx corner=%08lx", (unsigned long)center,
      (unsigned long)corner);
  /* center: resolved depth 0.25 rejects quad at 0.5 -> black;
   * corner: resolved depth 1.0 accepts it -> green */
  if (!px_eq(center, 0xFF000000) || !px_eq(corner, 0xFF00FF00)) {
    LOG("FAIL: depth resolve mismatch (want center FF000000 corner FF00FF00)");
    return 1;
  }
  LOG("ok: SAMPLE_ZERO depth resolve");

  LOG("PASS: MSAA color + depth-stencil resolve");

  /* keep presenting (suite runner kills the process) */
  CHECK(IDirect3DDevice9_SetDepthStencilSurface(dev, NULL));
  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_StretchRect(dev, rt1x, NULL, backbuf, NULL,
                                 D3DTEXF_NONE);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    frames++;
  }
done:
  LOG("exiting after %d frames", frames);
  return 0;
}
