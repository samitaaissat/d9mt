// consttest PS (constant * texture): interleaves a sampler so texture rebinds
// (descriptor-dirty) cross the constant path.
float4 color0 : register(c0);
sampler s0 : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  return color0 * tex2D(s0, uv);
}
