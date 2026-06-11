/* texquad: proves the texture + sampler path. CreateTexture/LockRect fills
 * a red/white checkerboard, SetTexture binds it, the PS samples it through
 * the sampler heap (UV range 0..2 shows D3DTADDRESS_WRAP).
 *
 * CX winewrapper swallows console output: results go to texquad_out.txt.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#include "texquad_vs_bytecode.h"
#include "texquad_ps_bytecode.h"

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
  float u, v;
};

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

int main(void) {
  g_out = fopen("texquad_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "texquad";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("texquad", "d9mt texquad",
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
  pp.hDeviceWindow = hwnd;

  IDirect3DDevice9 *dev = NULL;
  CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp,
                                &dev));

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  CHECK(IDirect3DDevice9_CreateVertexShader(
      dev, (const DWORD *)texquad_vs_bytecode, &vs));
  CHECK(IDirect3DDevice9_CreatePixelShader(
      dev, (const DWORD *)texquad_ps_bytecode, &ps));

  /* 64x64 A8R8G8B8 checkerboard, 8px cells, red/white */
  IDirect3DTexture9 *tex = NULL;
  CHECK(IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &tex, NULL));
  D3DLOCKED_RECT lr;
  CHECK(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0));
  for (int y = 0; y < 64; y++) {
    DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
    for (int x = 0; x < 64; x++)
      row[x] = ((x / 8 + y / 8) & 1) ? 0xFFFF0000 : 0xFFFFFFFF;
  }
  CHECK(IDirect3DTexture9_UnlockRect(tex, 0));

  static const D3DVERTEXELEMENT9 declElems[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,
       0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
       D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()};
  IDirect3DVertexDeclaration9 *decl = NULL;
  CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, declElems, &decl));

  /* NDC quad; UVs run to 2 so address-mode WRAP tiles the checker twice */
  static const struct Vertex verts[4] = {
      {-0.7f, 0.7f, 0.5f, 0.0f, 0.0f},
      {0.7f, 0.7f, 0.5f, 2.0f, 0.0f},
      {0.7f, -0.7f, 0.5f, 2.0f, 2.0f},
      {-0.7f, -0.7f, 0.5f, 0.0f, 2.0f},
  };
  static const WORD indices[6] = {0, 1, 2, 0, 2, 3};

  IDirect3DVertexBuffer9 *vb = NULL;
  CHECK(IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), 0, 0,
                                            D3DPOOL_DEFAULT, &vb, NULL));
  void *p = NULL;
  CHECK(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &p, 0));
  memcpy(p, verts, sizeof(verts));
  CHECK(IDirect3DVertexBuffer9_Unlock(vb));

  IDirect3DIndexBuffer9 *ib = NULL;
  CHECK(IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(indices), 0,
                                           D3DFMT_INDEX16, D3DPOOL_DEFAULT,
                                           &ib, NULL));
  CHECK(IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(indices), &p, 0));
  memcpy(p, indices, sizeof(indices));
  CHECK(IDirect3DIndexBuffer9_Unlock(ib));

  static const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1};
  CHECK(IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, mvp, 4));

  CHECK(IDirect3DDevice9_SetVertexDeclaration(dev, decl));
  CHECK(IDirect3DDevice9_SetVertexShader(dev, vs));
  CHECK(IDirect3DDevice9_SetPixelShader(dev, ps));
  CHECK(IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vertex)));
  CHECK(IDirect3DDevice9_SetIndices(dev, ib));
  CHECK(IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,
                                         D3DTADDRESS_WRAP));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,
                                         D3DTADDRESS_WRAP));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER,
                                         D3DTEXF_POINT));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER,
                                         D3DTEXF_POINT));

  LOG("entering render loop");
  int frames = 0;
  MSG msg;
  for (;;) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                           D3DCOLOR_XRGB(24, 24, 40), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    HRESULT hr = IDirect3DDevice9_DrawIndexedPrimitive(
        dev, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    frames++;
    if (frames == 3) {
      LOG("3 frames presented, DrawIndexedPrimitive hr=0x%08lx",
          (unsigned long)hr);
      LOG("PASS: texture + sampler path rendering");
    }
  }
done:
  LOG("exiting after %d frames", frames);
  return 0;
}
