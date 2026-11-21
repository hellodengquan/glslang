// SV_IsFrontFace maps to the SPIR-V FrontFacing built-in, which is only
// available in the fragment stage.  Using it as a vertex-stage input is an
// error rather than being silently lowered to a location variable.
struct VSOut {
    float4 pos : SV_Position;
};

VSOut main(bool isFrontFace : SV_IsFrontFace)
{
    VSOut outp;
    outp.pos = float4(isFrontFace ? 1.0 : 0.0, 0.0, 0.0, 1.0);
    return outp;
}
