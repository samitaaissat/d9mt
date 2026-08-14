/* consttest: shader-constant staleness readback test.
 *
 * The d9mt bind path snapshots constants into per-draw uploads gated by
 * dirty bits (per-stage descriptor dirty + DirtyPushData). A missed dirty
 * bit shows up as a draw rendered with the PREVIOUS draw's constants —
 * the suspected mechanism behind "sun specular constants stuck on the
 * wrong value" class bugs. This draws a row of quads where each case
 * changes exactly one thing between two consecutive draws (PS const only,
 * VS const only, PS const + texture, PS const + blend-toggle PSO swap,
 * PS const across a mid-scene Z-clear pass restart) and reads back the
 * result. Two-frame latch like depthbias.c (async PSO compile).
 *
 * Cells are 32px in a 256x64 backbuffer, quads 24x24 at cell origin+4.
 * Writes consttest_out.txt: one line per case + final PASS/FAIL line.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "consttest_vs_bytecode.h"
#include "consttest_psc_bytecode.h"
#include "consttest_pst_bytecode.h"

#define W 256
#define H 64
#define CELL 32
#define NCELLS 8

struct Vtx { float x, y, z; float u, v; };
#define FVF_CT (D3DFVF_XYZ | D3DFVF_TEX1)

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

/* clip-space quad covering cell c (24x24 px at cell*32+4) */
static void cell_quad(int c, struct Vtx v[6]) {
  float x0 = (c * CELL + 4) * 2.0f / W - 1.0f;
  float x1 = (c * CELL + 28) * 2.0f / W - 1.0f;
  float y0 = 1.0f - 4 * 2.0f / H;
  float y1 = 1.0f - 28 * 2.0f / H;
  struct Vtx q[6] = {
    {x0, y0, 0, 0, 0}, {x1, y0, 0, 1, 0}, {x0, y1, 0, 0, 1},
    {x0, y1, 0, 0, 1}, {x1, y0, 0, 1, 0}, {x1, y1, 0, 1, 1},
  };
  memcpy(v, q, sizeof(q));
}

static void set_ps_color(IDirect3DDevice9 *dev, float r, float g, float b) {
  float c[4] = {r, g, b, 1.0f};
  IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c, 1);
}

static IDirect3DTexture9 *solid_tex(IDirect3DDevice9 *dev, DWORD argb) {
  IDirect3DTexture9 *t = NULL;
  if (FAILED(IDirect3DDevice9_CreateTexture(dev, 8, 8, 1, 0, D3DFMT_A8R8G8B8,
                                            D3DPOOL_MANAGED, &t, NULL)))
    return NULL;
  D3DLOCKED_RECT lr;
  IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
  for (int y = 0; y < 8; y++) {
    DWORD *row = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
    for (int x = 0; x < 8; x++) row[x] = argb;
  }
  IDirect3DTexture9_UnlockRect(t, 0);
  return t;
}

static int close_to(DWORD px, DWORD want, int tol) {
  int dr = (int)((px >> 16) & 0xFF) - (int)((want >> 16) & 0xFF);
  int dg = (int)((px >> 8) & 0xFF) - (int)((want >> 8) & 0xFF);
  int db = (int)(px & 0xFF) - (int)(want & 0xFF);
  if (dr < 0) dr = -dr;
  if (dg < 0) dg = -dg;
  if (db < 0) db = -db;
  return dr <= tol && dg <= tol && db <= tol;
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "consttest";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("consttest", "consttest", WS_OVERLAPPEDWINDOW, 64,
                            64, W, H, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  FILE *out = fopen("consttest_out.txt", "w");
  if (!out) return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { fprintf(out, "FAIL: no d3d9\n"); fclose(out); return 1; }

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
                                     &pp, &dev)) || !dev) {
    fprintf(out, "FAIL: CreateDevice\n"); fclose(out); return 1;
  }

  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *psc = NULL, *pst = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)consttest_vs_bytecode, &vs)) ||
      FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)consttest_psc_bytecode, &psc)) ||
      FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)consttest_pst_bytecode, &pst))) {
    fprintf(out, "FAIL: shader creation\n"); fclose(out); return 1;
  }

  IDirect3DTexture9 *white = solid_tex(dev, 0xFFFFFFFF);
  IDirect3DTexture9 *gray = solid_tex(dev, 0xFF808080);
  if (!white || !gray) { fprintf(out, "FAIL: textures\n"); fclose(out); return 1; }

  IDirect3DSurface9 *sysmem = NULL;
  if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(
          dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL))) {
    fprintf(out, "FAIL: sysmem surface\n"); fclose(out); return 1;
  }

  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);

  struct Vtx q[6];
  float zero4[4] = {0, 0, 0, 0};
  float shift4[4] = {CELL * 2.0f / W, 0, 0, 0}; /* one cell to the right */

  int pass = 0;
  MSG msg;
  for (int frame = 0; frame < 300 && !pass; frame++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, FVF_CT);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, psc);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, zero4, 1);

    /* case A: PS const changes between consecutive draws, nothing else */
    set_ps_color(dev, 1, 0, 0);
    cell_quad(0, q);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));
    set_ps_color(dev, 0, 1, 0);
    cell_quad(1, q);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));

    /* case B: VS const only — same cell-1 geometry shifted into cell 2 */
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, shift4, 1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, zero4, 1);

    /* case C: PSO change (tex PS) + texture bind + PS const */
    IDirect3DDevice9_SetPixelShader(dev, pst);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)white);
    set_ps_color(dev, 0, 0, 1);
    cell_quad(3, q);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));

    /* case D: texture swap + PS const on same PSO */
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)gray);
    set_ps_color(dev, 1, 1, 0);
    cell_quad(4, q);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));

    /* case E: blend-toggle PSO swap + PS const (blend ONE/ZERO = no-op) */
    IDirect3DDevice9_SetPixelShader(dev, psc);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    set_ps_color(dev, 0, 1, 1);
    cell_quad(5, q);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);

    /* case F: PS const across a mid-scene Z-clear (deferred-clear restart) */
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    set_ps_color(dev, 1, 0, 1);
    cell_quad(6, q);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));

    /* case G: control */
    set_ps_color(dev, 1, 1, 1);
    cell_quad(7, q);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof(*q));

    IDirect3DDevice9_EndScene(dev);

    /* readback BEFORE Present (backbuffer contents valid) from frame 2 on */
    if (frame >= 2) {
      IDirect3DSurface9 *back = NULL;
      if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                                                   D3DBACKBUFFER_TYPE_MONO, &back))) {
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, back, sysmem))) {
          D3DLOCKED_RECT lr;
          if (SUCCEEDED(IDirect3DSurface9_LockRect(sysmem, &lr, NULL,
                                                   D3DLOCK_READONLY))) {
            static const DWORD want[NCELLS] = {
              0xFF0000, 0x00FF00, 0x00FF00, 0x0000FF,
              0x808000, 0x00FFFF, 0xFF00FF, 0xFFFFFF,
            };
            static const char *name[NCELLS] = {
              "A1 ps-const first", "A2 ps-const second", "B vs-const move",
              "C pso+tex",         "D tex-swap",         "E blend-pso-swap",
              "F clear-restart",   "G control",
            };
            int ok = 1, results[NCELLS];
            for (int c = 0; c < NCELLS; c++) {
              const DWORD *row = (const DWORD *)((const BYTE *)lr.pBits +
                                                 (16) * lr.Pitch);
              DWORD px = row[c * CELL + 16] & 0xFFFFFF;
              results[c] = close_to(px, want[c], 4);
              ok &= results[c];
              if (frame >= 299 || ok == 0) {
                /* only log detail on the final/failing frame */
              }
              if (frame == 299 || !results[c])
                fprintf(out, "%s cell=%d got=%06lX want=%06lX %s\n", name[c],
                        c, (unsigned long)px, (unsigned long)want[c],
                        results[c] ? "ok" : "STALE/WRONG");
            }
            if (ok) {
              fprintf(out, "PASS: all %d constant cases correct (frame %d)\n",
                      NCELLS, frame);
              pass = 1;
            } else if (frame == 299) {
              fprintf(out, "FAIL: constant staleness detected\n");
            }
            IDirect3DSurface9_UnlockRect(sysmem);
          }
        }
        IDirect3DSurface9_Release(back);
      }
    }

    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
  }

done:
  fflush(out);
  fclose(out);
  return pass ? 0 : 1;
}
