// SV_ClipDistance maps to the SPIR-V ClipDistance built-in, an array of float.
// An int element has no valid type for the built-in, even when buried inside a
// structure used as a vertex-stage output.
struct VSOut {
    float4 pos  : SV_Position;
    int    clip : SV_ClipDistance;
};

VSOut main()
{
    VSOut outp;
    outp.pos = float4(0.0, 0.0, 0.0, 1.0);
    outp.clip = 1;
    return outp;
}
