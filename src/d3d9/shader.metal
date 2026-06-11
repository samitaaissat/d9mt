#include <metal_stdlib>
using namespace metal;

// Vertex layout produced by the d3d9 layer: position already converted from
// D3DFVF_XYZRHW pixel coordinates to NDC on the CPU, color is raw D3DCOLOR
// (0xAARRGGBB little-endian dword).
struct VIn {
  packed_float4 pos;
  uint color;
};

struct VOut {
  float4 pos [[position]];
  float4 color;
};

vertex VOut vs_main(uint vid [[vertex_id]],
                    constant VIn *verts [[buffer(0)]]) {
  VIn v = verts[vid];
  VOut o;
  o.pos = float4(v.pos);
  o.color = float4(float((v.color >> 16) & 0xffu),
                   float((v.color >> 8) & 0xffu),
                   float(v.color & 0xffu),
                   float((v.color >> 24) & 0xffu)) / 255.0;
  return o;
}

fragment float4 ps_main(VOut in [[stage_in]]) {
  return in.color;
}

// Scaled blit (StretchRect with size change): fullscreen triangle that
// samples the source with linear filtering.
struct BlitOut {
  float4 pos [[position]];
  float2 uv;
};

vertex BlitOut blit_vs(uint vid [[vertex_id]]) {
  BlitOut o;
  float2 uv = float2((vid << 1) & 2, vid & 2);
  o.uv = uv;
  o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  return o;
}

fragment float4 blit_ps(BlitOut in [[stage_in]],
                        texture2d<float> src [[texture(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  return src.sample(s, in.uv);
}
