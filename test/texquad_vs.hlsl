// texquad VS: transform + UV passthrough
float4x4 mvp : register(c0);

struct VIn {
  float3 pos : POSITION;
  float2 uv : TEXCOORD0;
};

struct VOut {
  float4 pos : POSITION;
  float2 uv : TEXCOORD0;
};

VOut main(VIn i) {
  VOut o;
  o.pos = mul(float4(i.pos, 1.0), mvp);
  o.uv = i.uv;
  return o;
}
