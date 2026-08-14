// consttest VS: clip-space passthrough plus a c4 xy offset, so a VS-constant
// change between draws visibly moves the quad.
float4 off : register(c4);

struct VSIn  { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut main(VSIn i) {
  VSOut o;
  o.pos = float4(i.pos.xy + off.xy, 0.5, 1.0);
  o.uv  = i.uv;
  return o;
}
