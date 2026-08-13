/* depthbias: proves D3DRS_DEPTHBIAS carries D3D9 semantics — a raw offset
 * added to the fragment z (order 1e-4 in normalized depth), NOT the
 * Vulkan/Metal "least representable value" units the backend's
 * MTLRenderCommandEncoder setDepthBias expects. WoW's projected ground
 * textures re-draw terrain with a small negative constant bias; if the
 * bias arrives ~2^23 too small they z-fight (decals clip into terrain).
 *
 * Fixed-function XYZRHW quads, no shader blobs (verifytri pattern):
 *   1. green quad at z=0.5                      (base layer)
 *   2. red   quad at z=0.5+2e-6, DEPTHBIAS=-5e-4 -> wins under D3D9
 *      semantics (0.500002 - 0.0005 < 0.5), loses if the bias is nulled
 *   3. blue  quad at z=0.6, bias reset to 0      -> must lose (sanity:
 *      proves the depth test itself works, so a broken depth test can't
 *      masquerade as a pass)
 * Center pixel red => PASS; green => FAIL (bias ignored/underscaled);
 * blue or anything else => BROKEN depth test.
 * Results go to depthbias_out.txt; PASS/FAIL also on stdout. Self-exits. */
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

/* full-target quad at constant depth */
static void quad(struct Vertex v[6], float W, float H, float z, DWORD color) {
  const struct Vertex tl = {0.0f, 0.0f, z, 1.0f, color};
  const struct Vertex tr = {W, 0.0f, z, 1.0f, color};
  const struct Vertex bl = {0.0f, H, z, 1.0f, color};
  const struct Vertex br = {W, H, z, 1.0f, color};
  v[0] = tl; v[1] = tr; v[2] = bl;
  v[3] = bl; v[4] = tr; v[5] = br;
}

int main(void) {
  const UINT W = 256, H = 256;

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

  const float biasNeg = -0.0005f; /* D3D9: raw z offset, wins over +2e-6 */
  const float biasZero = 0.0f;
  DWORD biasNegBits, biasZeroBits;
  memcpy(&biasNegBits, &biasNeg, 4);
  memcpy(&biasZeroBits, &biasZero, 4);

  struct Vertex v[6];
  int frames = 0, verified = 0, ok = 0;
  DWORD prevRgb = 0xFFFFFFFF; /* impossible XRGB value: no match on frame 1 */
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

    /* 1: green base layer at z=0.5 */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZeroBits));
    quad(v, (float)W, (float)H, 0.5f, 0xFF00FF00);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    /* 2: red decal slightly BEHIND, pulled in front by the constant bias */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasNegBits));
    quad(v, (float)W, (float)H, 0.500002f, 0xFFFF0000);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    /* 3: blue sanity quad clearly behind, bias reset — must stay hidden */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, biasZeroBits));
    quad(v, (float)W, (float)H, 0.6f, 0xFF0000FF);
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

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
      const DWORD px =
          *(const DWORD *)((const BYTE *)lr.pBits + (H / 2) * lr.Pitch +
                           (W / 2) * 4);
      IDirect3DSurface9_UnlockRect(sysmem);

      const DWORD rgb = px & 0x00FFFFFF;
      /* Latch only after TWO consecutive frames agree: all three quads
       * share one PSO, and the async compile can flip it hot between two
       * draws of a single frame — that transition frame can show any
       * subset of the layers (e.g. red-on-clear = false PASS, or
       * blue-only = false BROKEN). The frame after the flip has every
       * draw live, so agreement across two frames screens the race out. */
      if (rgb != 0x00101010 && rgb == prevRgb) { /* stable — classify */
        verified = 1;
        LOG("center pixel: 0x%08lx (frame %d)", (unsigned long)px, frames);
        if (rgb == 0x00FF0000) {
          LOG("PASS: DEPTHBIAS applied with D3D9 raw-offset semantics");
          ok = 1;
        } else if (rgb == 0x0000FF00) {
          LOG("FAIL: DEPTHBIAS ignored or underscaled (decal lost to base "
              "layer)");
        } else {
          LOG("FAIL: BROKEN depth test (expected red or green, got 0x%06lx)",
              (unsigned long)rgb);
        }
      } else if (frames >= 600) {
        verified = 1;
        LOG("FAIL: no stable frame in %d frames (pipeline never ready?)",
            frames);
      } else {
        prevRgb = rgb;
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
