// SV_IsFrontFace maps to the SPIR-V FrontFacing built-in, which requires a
// scalar bool.  A float declaration keeps no valid type for the built-in.
float4 main(float isFrontFace : SV_IsFrontFace) : SV_Target0
{
    return float4(isFrontFace, 0.0, 0.0, 1.0);
}
