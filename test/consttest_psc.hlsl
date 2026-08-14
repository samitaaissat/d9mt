// consttest PS (constant only): output = c0. A stale c0 between draws is the
// exact failure mode of a missed push/cbuffer dirty bit.
float4 color0 : register(c0);

float4 main() : COLOR {
  return color0;
}
