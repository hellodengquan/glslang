// Test that mixed HLSL resource declarations (texture arrays, sampler arrays,
// structured buffers and UAVs) with explicit registers and explicit space keep
// their per-register-class binding spaces in the intermediate representation,
// and that resources without explicit registers are auto-assigned within their
// own register class without colliding with (or being overwritten by) the
// explicit declarations, including the register spans of resource arrays.

Texture2D<float4>        texArr[3] : register(t1, space1); // t1..t3 in set 1
SamplerState             sampArr[2] : register(s1, space1); // s1..s2 in set 1
StructuredBuffer<float4> sb        : register(t4, space1);
RWTexture2D<float4>      rwTex     : register(u0, space2);
cbuffer MyCB : register(b0, space1) { float4 cbData; }

// No explicit registers: auto-assigned per register class.
Texture2D<float4>        texAuto;
SamplerState             sampAuto;
StructuredBuffer<float4> sbAuto;
RWStructuredBuffer<float> rwAuto;

float4 main() : SV_Target0
{
    float4 a = texArr[2].Sample(sampArr[1], float2(0, 0));
    float4 b = sb[0];
    float4 c = texAuto.Sample(sampAuto, float2(0, 0));
    float4 d = sbAuto[0];
    rwAuto[0] = cbData.x;
    return a + b + c + d + rwTex[uint2(0, 0)] + rwAuto[0];
}
