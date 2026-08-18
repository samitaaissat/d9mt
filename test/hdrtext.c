/* hdrtext: on-screen probe for the "text is ugly under HDR" report.
 *
 * Renders a diagnostic card 1:1 texel-to-pixel (point-sampled, client rect
 * sized exactly to the backbuffer so the present sample pass has no scaling
 * work to do):
 *   - GDI grayscale-antialiased white text (WoW chat-size and larger)
 *   - the same text NONANTIALIASED
 *   - a 1px vertical hairline comb (any resample in the present chain turns
 *     it into gray mush — geometric-blur detector)
 *   - solid white / 50% gray / 25% gray patches (tonal measurement)
 * over a dark background, presenting for HDRTEXT_SECONDS (default 15).
 *
 * Run once with D9MT_HDR=0 and once with D9MT_HDR=1 D9MT_HDR_PEAK=4 and
 * screenshot the window: if the hairlines stay crisp in both, the present
 * chain is geometry-clean and the complaint is tonal (the inverse tone map
 * exaggerating antialiased edge contrast); if they mush, there is a real
 * resample. Writes hdrtext_out.txt (READY marker + window position) so a
 * harness knows when and where to capture. Self-exits. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

struct Vertex {
  float x, y, z, rhw;
  float u, v;
};
#define FVF_TEX (D3DFVF_XYZRHW | D3DFVF_TEX1)

struct ColorVertex {
  float x, y, z, rhw;
  DWORD color;
};
#define FVF_COLOR (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

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

static void drawTextRow(HDC dc, int y, const char *face, int height,
                        DWORD quality, const char *label) {
  HFONT font = CreateFontA(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, quality,
                           DEFAULT_PITCH | FF_DONTCARE, face);
  HFONT old = (HFONT)SelectObject(dc, font);
  TextOutA(dc, 8, y, label, (int)strlen(label));
  SelectObject(dc, old);
  DeleteObject(font);
}

int main(void) {
  const UINT W = 512, H = 320;
  int seconds = 15;
  {
    const char *s = getenv("HDRTEXT_SECONDS");
    if (s && atoi(s) > 0)
      seconds = atoi(s);
  }

  g_out = fopen("hdrtext_out.txt", "w");
  if (!g_out)
    return 1;

  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "hdrtext";
  RegisterClassA(&wc);

  /* client rect must equal the backbuffer exactly, or the present sample
   * pass linear-rescales and contaminates the geometric measurement */
  RECT r = {0, 0, (LONG)W, (LONG)H};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("hdrtext", "d9mt hdrtext", WS_OVERLAPPEDWINDOW,
                            64, 64, r.right - r.left, r.bottom - r.top, NULL,
                            NULL, wc.hInstance, NULL);
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

  /* ---- build the diagnostic card in a GDI DIB ---- */
  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = (LONG)W;
  bi.bmiHeader.biHeight = -(LONG)H; /* top-down */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *bits = NULL;
  HDC screen = GetDC(NULL);
  HDC memdc = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(memdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  ReleaseDC(NULL, screen);
  if (!dib || !bits) {
    LOG("FAIL: CreateDIBSection");
    return 1;
  }
  SelectObject(memdc, dib);

  /* dark WoW-ish background */
  {
    RECT full = {0, 0, (LONG)W, (LONG)H};
    HBRUSH bg = CreateSolidBrush(RGB(24, 28, 36));
    FillRect(memdc, &full, bg);
    DeleteObject(bg);
  }
  SetBkMode(memdc, TRANSPARENT);
  SetTextColor(memdc, RGB(255, 255, 255));
  drawTextRow(memdc, 8, "Tahoma", 12, ANTIALIASED_QUALITY,
              "AA 12px: The quick brown fox jumps over the lazy dog 0123456789");
  drawTextRow(memdc, 28, "Tahoma", 12, NONANTIALIASED_QUALITY,
              "NO 12px: The quick brown fox jumps over the lazy dog 0123456789");
  drawTextRow(memdc, 48, "Tahoma", 18, ANTIALIASED_QUALITY,
              "AA 18px: Sample HDR text rendering");
  drawTextRow(memdc, 76, "Tahoma", 18, NONANTIALIASED_QUALITY,
              "NO 18px: Sample HDR text rendering");
  SetTextColor(memdc, RGB(255, 210, 0)); /* WoW gold */
  drawTextRow(memdc, 104, "Tahoma", 14, ANTIALIASED_QUALITY,
              "GOLD 14px AA: Quest tracker style text sample");
  GdiFlush();

  /* hairline comb + tonal patches written directly into the DIB */
  {
    DWORD *px = (DWORD *)bits;
    UINT x, y;
    for (y = 140; y < 180; y++)
      for (x = 8; x < W - 8; x++)
        px[y * W + x] = (x & 1) ? 0x00FFFFFF : 0x00000000; /* 1px comb */
    for (y = 190; y < 230; y++)
      for (x = 8; x < W - 8; x++) { /* 2px comb */
        px[y * W + x] = ((x >> 1) & 1) ? 0x00FFFFFF : 0x00000000;
      }
    for (y = 240; y < 300; y++) {
      for (x = 8; x < 128; x++)
        px[y * W + x] = 0x00FFFFFF; /* solid white */
      for (x = 136; x < 256; x++)
        px[y * W + x] = 0x00BCBCBC; /* ~50% linear (188 sRGB) */
      for (x = 264; x < 384; x++)
        px[y * W + x] = 0x00898989; /* ~25% linear (137 sRGB) */
      for (x = 392; x < W - 8; x++)
        px[y * W + x] = (DWORD)(((x - 392) * 255) / (W - 8 - 392)) * 0x010101;
    }
    /* opaque alpha everywhere */
    for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
        px[y * W + x] |= 0xFF000000;
  }

  /* upload into a MANAGED texture */
  IDirect3DTexture9 *tex = NULL;
  CHECK(IDirect3DDevice9_CreateTexture(dev, W, H, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &tex, NULL));
  {
    D3DLOCKED_RECT lr;
    CHECK(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0));
    UINT y;
    for (y = 0; y < H; y++)
      memcpy((BYTE *)lr.pBits + y * lr.Pitch, (BYTE *)bits + y * W * 4, W * 4);
    CHECK(IDirect3DTexture9_UnlockRect(tex, 0));
  }

  /* UI overlay texture: white AA text with alpha = coverage (straight
   * alpha), transparent background — the WoW interface pattern. GDI won't
   * write alpha, so render white-on-black and lift luminance into alpha. */
  const UINT UIW = 512, UIH = 40;
  IDirect3DTexture9 *uiTex = NULL;
  CHECK(IDirect3DDevice9_CreateTexture(dev, UIW, UIH, 1, 0, D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED, &uiTex, NULL));
  {
    RECT full = {0, 0, (LONG)UIW, (LONG)UIH};
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memdc, &full, bg); /* reuse the DIB's top rows as scratch */
    DeleteObject(bg);
    SetTextColor(memdc, RGB(255, 255, 255));
    drawTextRow(memdc, 4, "Tahoma", 13, ANTIALIASED_QUALITY,
                "UI OVERLAY: chat text over the world, SDR-anchored under HDR");
    drawTextRow(memdc, 21, "Tahoma", 12, ANTIALIASED_QUALITY,
                "The quick brown fox jumps over the lazy dog 0123456789");
    GdiFlush();
    D3DLOCKED_RECT lr;
    CHECK(IDirect3DTexture9_LockRect(uiTex, 0, &lr, NULL, 0));
    UINT x, y;
    for (y = 0; y < UIH; y++) {
      const DWORD *srcRow = (const DWORD *)bits + y * W;
      DWORD *dstRow = (DWORD *)((BYTE *)lr.pBits + y * lr.Pitch);
      for (x = 0; x < UIW; x++) {
        DWORD cov = srcRow[x] & 0xFF; /* blue channel = luminance of white */
        dstRow[x] = (cov << 24) | 0x00FFFFFF;
      }
    }
    CHECK(IDirect3DTexture9_UnlockRect(uiTex, 0));
  }

  CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER,
                                         D3DTEXF_POINT));
  CHECK(IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER,
                                         D3DTEXF_POINT));

  /* D3D9 texel alignment: shift screen coords by -0.5 for 1:1 mapping */
  struct Vertex v[6] = {
      {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
      {W - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
      {-0.5f, H - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
      {-0.5f, H - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
      {W - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
      {W - 0.5f, H - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f},
  };
  /* UI text overlay quad: over the tonal patches, y 240..280 */
  struct Vertex uiv[6] = {
      {-0.5f, 239.5f, 0.0f, 1.0f, 0.0f, 0.0f},
      {UIW - 0.5f, 239.5f, 0.0f, 1.0f, 1.0f, 0.0f},
      {-0.5f, 239.5f + UIH, 0.0f, 1.0f, 0.0f, 1.0f},
      {-0.5f, 239.5f + UIH, 0.0f, 1.0f, 0.0f, 1.0f},
      {UIW - 0.5f, 239.5f, 0.0f, 1.0f, 1.0f, 0.0f},
      {UIW - 0.5f, 239.5f + UIH, 0.0f, 1.0f, 1.0f, 1.0f},
  };
  /* opaque white UI panel at (400..440, 20..52): tag probe expects 255 */
  struct ColorVertex panel[6] = {
      {399.5f, 19.5f, 0.0f, 1.0f, 0xFFFFFFFF},
      {439.5f, 19.5f, 0.0f, 1.0f, 0xFFFFFFFF},
      {399.5f, 51.5f, 0.0f, 1.0f, 0xFFFFFFFF},
      {399.5f, 51.5f, 0.0f, 1.0f, 0xFFFFFFFF},
      {439.5f, 19.5f, 0.0f, 1.0f, 0xFFFFFFFF},
      {439.5f, 51.5f, 0.0f, 1.0f, 0xFFFFFFFF},
  };
  /* world-transparent control at (470..505, 100..130): z-test ON + blend —
   * must NOT be classified UI, tag probe expects 0 */
  struct ColorVertex worldBlend[6] = {
      {469.5f, 99.5f, 0.5f, 1.0f, 0x80FF4040},
      {504.5f, 99.5f, 0.5f, 1.0f, 0x80FF4040},
      {469.5f, 129.5f, 0.5f, 1.0f, 0x80FF4040},
      {469.5f, 129.5f, 0.5f, 1.0f, 0x80FF4040},
      {504.5f, 99.5f, 0.5f, 1.0f, 0x80FF4040},
      {504.5f, 129.5f, 0.5f, 1.0f, 0x80FF4040},
  };

  /* tag asserts only make sense when HDR + the UI tag are on */
  const char *envHdr = getenv("D9MT_HDR");
  const char *envUi = getenv("D9MT_HDR_UI");
  const int wantTagChecks = envHdr
                         && (strcmp(envHdr, "1") == 0 || strcmp(envHdr, "force") == 0)
                         && (!envUi || strcmp(envUi, "0") != 0);
  IDirect3DSurface9 *sysmem = NULL;
  CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(
      dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL));
  int tagChecked = 0, tagOk = 0;

  DWORD start = GetTickCount();
  int frames = 0, announced = 0;
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
                                 D3DCOLOR_XRGB(10, 10, 12), 1.0f, 0));
    CHECK(IDirect3DDevice9_BeginScene(dev));

    /* world: the card, depth-tested, opaque */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE));
    CHECK(IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex));
    CHECK(IDirect3DDevice9_SetFVF(dev, FVF_TEX));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, v,
                                           sizeof(struct Vertex)));

    /* world-transparent control: blended but depth-tested — not UI */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
    CHECK(IDirect3DDevice9_SetTexture(dev, 0, NULL));
    CHECK(IDirect3DDevice9_SetFVF(dev, FVF_COLOR));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2,
                                           worldBlend, sizeof(struct ColorVertex)));

    /* UI: depth test off + blended — the interface pattern */
    CHECK(IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2,
                                           panel, sizeof(struct ColorVertex)));
    CHECK(IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)uiTex));
    CHECK(IDirect3DDevice9_SetFVF(dev, FVF_TEX));
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, uiv,
                                           sizeof(struct Vertex)));

    CHECK(IDirect3DDevice9_EndScene(dev));
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    /* tag verification: the backbuffer's spare alpha byte must carry the
     * UI coverage (panel 255, world 0) once the tag machinery is live.
     * Poll rather than assert at a fixed frame: the HDR gate may latch a
     * premature SDR verdict and re-run itself on the 32-present retry
     * cadence, so activation can land a second or two into the run. */
    if (wantTagChecks && frames >= 10 && !tagChecked) {
      IDirect3DSurface9 *bb = NULL;
      CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0,
                                           D3DBACKBUFFER_TYPE_MONO, &bb));
      CHECK(IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem));
      IDirect3DSurface9_Release(bb);
      D3DLOCKED_RECT lr;
      CHECK(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY));
      const BYTE aWorld = ((const BYTE *)lr.pBits)[260 * lr.Pitch + 60 * 4 + 3];
      const BYTE aPanel = ((const BYTE *)lr.pBits)[36 * lr.Pitch + 420 * 4 + 3];
      const BYTE aWBlend = ((const BYTE *)lr.pBits)[115 * lr.Pitch + 487 * 4 + 3];
      IDirect3DSurface9_UnlockRect(sysmem);
      if (aWorld == 0 && aPanel >= 250 && aWBlend == 0) {
        tagChecked = 1;
        tagOk = 1;
        LOG("tag bytes: world=%u panel=%u worldBlend=%u (frame %d)",
            aWorld, aPanel, aWBlend, frames);
        LOG("PASS: UI coverage tag (world clean, panel saturated, "
            "z-on blend untagged)");
      } else if (frames >= 500) {
        tagChecked = 1;
        LOG("tag bytes: world=%u panel=%u worldBlend=%u (frame %d)",
            aWorld, aPanel, aWBlend, frames);
        LOG("FAIL: UI coverage tag bytes off (want world=0 panel>=250 "
            "worldBlend=0) — HDR gate never engaged, or tag plumbing broken");
      }
    }

    if (++frames == 5 && !announced) {
      announced = 1;
      RECT wr, cr;
      POINT cl = {0, 0};
      GetWindowRect(hwnd, &wr);
      GetClientRect(hwnd, &cr);
      ClientToScreen(hwnd, &cl);
      LOG("READY client_screen=%ld,%ld client=%ldx%ld window=%ld,%ld,%ld,%ld",
          (long)cl.x, (long)cl.y, (long)(cr.right - cr.left),
          (long)(cr.bottom - cr.top), (long)wr.left, (long)wr.top,
          (long)wr.right, (long)wr.bottom);
    }
    if (GetTickCount() - start > (DWORD)(seconds * 1000))
      break;
  }

done:
  LOG("DONE frames=%d", frames);
  fclose(g_out);
  return (wantTagChecks && !tagOk) ? 1 : 0;
}
