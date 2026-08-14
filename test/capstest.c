/* capstest: dump the D3DCAPS9 fields (and format probes) WoW 3.3.5 uses to
 * pick its shader path / effect permutations, as a diffable text file.
 * Run under two renderers and diff the outputs to find divergences that
 * change what the game renders (e.g. terrain specular permutation choice). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const struct { D3DFORMAT fmt; const char *name; DWORD usage; D3DRESOURCETYPE rtype; } PROBES[] = {
  { D3DFMT_A8R8G8B8,  "A8R8G8B8-tex",  0, D3DRTYPE_TEXTURE },
  { D3DFMT_A8R8G8B8,  "A8R8G8B8-rt",   D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE },
  { D3DFMT_DXT1,      "DXT1",          0, D3DRTYPE_TEXTURE },
  { D3DFMT_DXT3,      "DXT3",          0, D3DRTYPE_TEXTURE },
  { D3DFMT_DXT5,      "DXT5",          0, D3DRTYPE_TEXTURE },
  { D3DFMT_D24S8,     "D24S8-ds",      D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
  { D3DFMT_D24X8,     "D24X8-ds",      D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
  { D3DFMT_D16,       "D16-ds",        D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE },
  { D3DFMT_D24S8,     "D24S8-dstex",   D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE },
  { D3DFMT_A16B16G16R16F, "A16F-rt",   D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE },
  { D3DFMT_R32F,      "R32F-rt",       D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE },
  { D3DFMT_G16R16,    "G16R16-tex",    0, D3DRTYPE_TEXTURE },
  { D3DFMT_A2B10G10R10, "A2B10G10R10-rt", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE },
  { (D3DFORMAT)MAKEFOURCC('I','N','T','Z'), "INTZ", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE },
  { (D3DFORMAT)MAKEFOURCC('N','U','L','L'), "NULL", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE },
};

int main(void) {
  FILE *out = fopen("capstest_out.txt", "w");
  if (!out) return 1;

  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) { fprintf(out, "no d3d9\n"); fclose(out); return 1; }

  D3DADAPTER_IDENTIFIER9 ident;
  if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &ident)))
    fprintf(out, "driver: %s | desc: %s | vendor %04lX dev %04lX\n",
            ident.Driver, ident.Description,
            (unsigned long)ident.VendorId, (unsigned long)ident.DeviceId);

  D3DCAPS9 c;
  if (FAILED(IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &c))) {
    fprintf(out, "GetDeviceCaps FAILED\n"); fclose(out); return 1;
  }

#define F(x) fprintf(out, #x " = 0x%08lX\n", (unsigned long)c.x)
  F(Caps); F(Caps2); F(Caps3); F(PresentationIntervals);
  F(DevCaps); F(PrimitiveMiscCaps); F(RasterCaps); F(ZCmpCaps);
  F(SrcBlendCaps); F(DestBlendCaps); F(AlphaCmpCaps); F(ShadeCaps);
  F(TextureCaps); F(TextureFilterCaps); F(TextureAddressCaps);
  F(LineCaps); F(StencilCaps); F(FVFCaps); F(TextureOpCaps);
  F(VertexProcessingCaps); F(DevCaps2); F(DeclTypes);
  fprintf(out, "MaxTextureWidth/Height = %lu/%lu\n",
          (unsigned long)c.MaxTextureWidth, (unsigned long)c.MaxTextureHeight);
  fprintf(out, "MaxSimultaneousTextures = %lu\n", (unsigned long)c.MaxSimultaneousTextures);
  fprintf(out, "MaxTextureBlendStages = %lu\n", (unsigned long)c.MaxTextureBlendStages);
  fprintf(out, "MaxActiveLights = %lu\n", (unsigned long)c.MaxActiveLights);
  fprintf(out, "MaxUserClipPlanes = %lu\n", (unsigned long)c.MaxUserClipPlanes);
  fprintf(out, "MaxVertexBlendMatrices = %lu\n", (unsigned long)c.MaxVertexBlendMatrices);
  fprintf(out, "MaxAnisotropy = %lu\n", (unsigned long)c.MaxAnisotropy);
  fprintf(out, "MaxPrimitiveCount = %lu\n", (unsigned long)c.MaxPrimitiveCount);
  fprintf(out, "MaxStreams = %lu\n", (unsigned long)c.MaxStreams);
  fprintf(out, "VertexShaderVersion = 0x%08lX\n", (unsigned long)c.VertexShaderVersion);
  fprintf(out, "MaxVertexShaderConst = %lu\n", (unsigned long)c.MaxVertexShaderConst);
  fprintf(out, "PixelShaderVersion = 0x%08lX\n", (unsigned long)c.PixelShaderVersion);
  fprintf(out, "PixelShader1xMaxValue = %f\n", c.PixelShader1xMaxValue);
  fprintf(out, "NumSimultaneousRTs = %lu\n", (unsigned long)c.NumSimultaneousRTs);
  fprintf(out, "MaxVShaderInstructionsExecuted = %lu\n", (unsigned long)c.MaxVShaderInstructionsExecuted);
  fprintf(out, "MaxPShaderInstructionsExecuted = %lu\n", (unsigned long)c.MaxPShaderInstructionsExecuted);
  fprintf(out, "VS20 dynflow/temps/staticflow = %ld/%ld/%ld\n",
          (long)c.VS20Caps.DynamicFlowControlDepth, (long)c.VS20Caps.NumTemps,
          (long)c.VS20Caps.StaticFlowControlDepth);
  fprintf(out, "PS20 caps/dynflow/temps/staticflow/instrslots = 0x%lX/%ld/%ld/%ld/%ld\n",
          (unsigned long)c.PS20Caps.Caps, (long)c.PS20Caps.DynamicFlowControlDepth,
          (long)c.PS20Caps.NumTemps, (long)c.PS20Caps.StaticFlowControlDepth,
          (long)c.PS20Caps.NumInstructionSlots);

  for (unsigned i = 0; i < sizeof(PROBES) / sizeof(PROBES[0]); i++) {
    HRESULT hr = IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL,
        D3DFMT_X8R8G8B8, PROBES[i].usage, PROBES[i].rtype, PROBES[i].fmt);
    fprintf(out, "fmt %-16s = %s\n", PROBES[i].name, SUCCEEDED(hr) ? "OK" : "NO");
  }

  fclose(out);
  return 0;
}
