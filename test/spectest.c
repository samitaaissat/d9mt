/* spectest: end-to-end numeric fidelity test of WoW's terrain sun-specular
 * path, using the REAL client shaders (terrain vs_2_0 permutation 4 +
 * terrain1 ps_2_0) extracted from the 3.3.5 client.
 *
 * The terrain gloss is: VS computes spec = pow(max(N.H, 0), c27.w) * c27.rgb
 * (H = normalize(normalize(-viewPos) + c24)), PS outputs
 * diffuse*2*shadow*v0 + tex0.a * v1 * tex1.a. With ambient/diffuse light
 * zeroed and white textures the framebuffer IS the specular term, so any
 * translation infidelity (fast-math powr edge cases, nrm guards, strictPow)
 * shows up as a numeric mismatch against the CPU reference.
 *
 * Renders 8 quads with per-quad constant normals sweeping N.H in [0, 1],
 * repeated for exponents {0, 2, 20, 76}; reads back and compares. Exponent 0
 * checks strictPow (pow(x,0)=1 for ALL x including 0). Writes
 * spectest_out.txt; PASS iff every cell within tolerance. Two-frame latch.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "spectest_vs_bytecode.h"  /* terrain vs20 permutation 4 */
#include "spectest_ps_bytecode.h"  /* terrain1 ps20 */

#define W 256
#define H 64
#define CELL 32
#define NCELLS 8

struct Vtx { float x, y, z; float nx, ny, nz; };
#define FVF_SPEC (D3DFVF_XYZ | D3DFVF_NORMAL)

static const float EXPS[] = {0.0f, 2.0f, 20.0f, 76.0f};
#define NEXP 4

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
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

/* per-quad normal choices sweeping N.H (normal tilts away from view/light) */
static void cell_normal(int c, float *nx, float *ny, float *nz) {
  /* angles chosen to give a spread of N.H values incl. exactly facing (1.0)
   * and fully away (<= 0) */
  static const float ang[NCELLS] = {0.0f, 0.15f, 0.30f, 0.45f,
                                    0.70f, 1.00f, 1.35f, 2.20f};
  *nx = sinf(ang[c]);
  *ny = 0.0f;
  *nz = -cosf(ang[c]);
}

int main(void) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "spectest";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowA("spectest", "spectest", WS_OVERLAPPEDWINDOW, 64,
                            64, W, H, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  FILE *out = fopen("spectest_out.txt", "w");
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
  IDirect3DPixelShader9 *ps = NULL;
  if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)spectest_vs_bytecode, &vs)) ||
      FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)spectest_ps_bytecode, &ps))) {
    fprintf(out, "FAIL: shader creation\n"); fclose(out); return 1;
  }

  IDirect3DTexture9 *white = solid_tex(dev, 0xFFFFFFFF);
  IDirect3DSurface9 *sysmem = NULL;
  IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8,
                                               D3DPOOL_SYSTEMMEM, &sysmem, NULL);
  if (!white || !sysmem) { fprintf(out, "FAIL: resources\n"); fclose(out); return 1; }

  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
  IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);

  /* ---- VS constants (see disassembly in test header) ----
   * c0..c2 world->view columns + c3 translate: identity (viewPos = objPos)
   * c4..c7 projection columns: screen-map x,y; z -> 0.5
   * c12 fog params (unused, fog output ignored), c13/c14/c18/c19/c23 uv
   * c24 light dir (toward light), c25 ambient=0, c26 diffuse=0,
   * c27 spec color rgb + exponent w                                     */
  static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, ident, 4);
  /* projection: oPos = viewPos.x*c4 + viewPos.y*c5 + viewPos.z*c6 + w*c7
   * want clip.x = (objX/128 - 1), clip.y = (1 - objY/32), clip.z = 0.5:
   * c4 = (1/128, 0, 0, 0); c5 = (0, -1/32, 0, 0); c6 = 0; c7 = (-1+ox, 1+oy, .5, 1)
   * BUT viewPos.z = objZ = 200 (quads placed at z=200 so viewDir ~= (0,0,-1));
   * c6 must not leak z into x/y: c6 = (0,0,0,0). */
  {
    float proj[16] = {
      1.0f/128.0f, 0, 0, 0,
      0, -1.0f/32.0f, 0, 0,
      0, 0, 0, 0,
      -1.0f, 1.0f, 0.5f, 1.0f,
    };
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, proj, 4);
  }
  {
    float zero[4*16] = {0};
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 12, zero, 16); /* c12..c27 zero */
  }
  { float l[4] = {0, 0, -1, 0};  IDirect3DDevice9_SetVertexShaderConstantF(dev, 24, l, 1); }

  struct Vtx q[NCELLS][6];
  for (int c = 0; c < NCELLS; c++) {
    float nx, ny, nz;
    cell_normal(c, &nx, &ny, &nz);
    float x0 = (float)(c * CELL + 4), x1 = (float)(c * CELL + 28);
    float y0 = 4.0f, y1 = 28.0f;
    struct Vtx t[6] = {
      {x0, y0, 200.0f, nx, ny, nz}, {x1, y0, 200.0f, nx, ny, nz},
      {x0, y1, 200.0f, nx, ny, nz}, {x0, y1, 200.0f, nx, ny, nz},
      {x1, y0, 200.0f, nx, ny, nz}, {x1, y1, 200.0f, nx, ny, nz},
    };
    memcpy(q[c], t, sizeof(t));
  }

  int pass_total = 0, latched = 0;
  MSG msg;
  for (int frame = 0; frame < 90 && !latched; frame++) {
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    int all_ok = 1;
    int checked = 0;
    for (int e = 0; e < NEXP; e++) {
      IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                             D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
      IDirect3DDevice9_BeginScene(dev);
      IDirect3DDevice9_SetFVF(dev, FVF_SPEC);
      IDirect3DDevice9_SetVertexShader(dev, vs);
      IDirect3DDevice9_SetPixelShader(dev, ps);
      IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)white);
      IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)white);
      float c27[4] = {1.0f, 1.0f, 1.0f, EXPS[e]};
      IDirect3DDevice9_SetVertexShaderConstantF(dev, 27, c27, 1);
      for (int c = 0; c < NCELLS; c++)
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q[c],
                                         sizeof(struct Vtx));
      IDirect3DDevice9_EndScene(dev);

      if (frame >= 2) {
        IDirect3DSurface9 *back = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                                                     D3DBACKBUFFER_TYPE_MONO, &back))) {
          if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, back, sysmem))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(IDirect3DSurface9_LockRect(sysmem, &lr, NULL,
                                                     D3DLOCK_READONLY))) {
              checked++;
              for (int c = 0; c < NCELLS; c++) {
                /* CPU reference. The VS computes spec PER-VERTEX and the GPU
                 * Gouraud-interpolates the color; the cell-center pixel
                 * (16,16) sits on the quad diagonal, i.e. exactly midway
                 * between vertices (28,4) and (4,28) — so the reference is
                 * the mean of the per-vertex spec at those two corners. */
                float nx, ny, nz;
                cell_normal(c, &nx, &ny, &nz);
                double spec = 0.0, ndh_dbg = 0.0;
                const double corner[2][2] = {{c * CELL + 28.0, 4.0},
                                             {c * CELL + 4.0, 28.0}};
                for (int k = 0; k < 2; k++) {
                  double px = corner[k][0], py = corner[k][1], vz = 200.0;
                  /* shader: H = normalize(normalize(-viewPos) + L) */
                  double ivx = -px, ivy = -py, ivz = -vz;
                  double il = sqrt(ivx*ivx + ivy*ivy + ivz*ivz);
                  double hx = ivx/il, hy = ivy/il, hz = ivz/il + (-1.0);
                  double hl = sqrt(hx*hx + hy*hy + hz*hz);
                  hx/=hl; hy/=hl; hz/=hl;
                  double ndh = nx*hx + ny*hy + nz*hz;
                  if (ndh < 0) ndh = 0;
                  ndh_dbg += 0.5 * ndh;
                  double s;
                  if (EXPS[e] == 0.0f) s = 1.0;    /* d3d9: pow(x,0)=1 */
                  else s = pow(ndh, (double)EXPS[e]);
                  if (s < 0) s = 0;
                  if (s > 1) s = 1;
                  spec += 0.5 * s;
                }
                double ndh = ndh_dbg;
                int want = (int)(spec * 255.0 + 0.5);

                const DWORD *row = (const DWORD *)((const BYTE *)lr.pBits + 16 * lr.Pitch);
                int got = (int)((row[c * CELL + 16] >> 8) & 0xFF); /* green ch */
                int diff = got - want; if (diff < 0) diff = -diff;
                /* tolerance: 8-bit quantize + interpolated view-vector spread
                 * across the cell + fast-math noise */
                int ok = diff <= 6;
                all_ok &= ok;
                if (!ok && frame >= 60)
                  fprintf(out, "exp=%g cell=%d ndh=%.4f got=%d want=%d MISMATCH\n",
                          EXPS[e], c, ndh, got, want);
              }
              IDirect3DSurface9_UnlockRect(sysmem);
            }
          }
          IDirect3DSurface9_Release(back);
        }
      }
      IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    if (frame >= 2 && checked == NEXP && all_ok) {
      fprintf(out, "PASS: terrain specular numerically faithful "
                   "(%d exponents x %d N.H cells, frame %d)\n",
              NEXP, NCELLS, frame);
      pass_total = 1;
      latched = 1;
    } else if (frame == 89 && !all_ok) {
      fprintf(out, "FAIL: specular mismatches (see lines above)\n");
    }
  }

done:
  fflush(out);
  fclose(out);
  return pass_total ? 0 : 1;
}
