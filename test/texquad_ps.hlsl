// texquad PS: plain texture sample
sampler2D s0 : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  return tex2D(s0, uv);
}
