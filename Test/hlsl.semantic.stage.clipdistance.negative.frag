// SV_ClipDistance maps to the SPIR-V ClipDistance built-in, which cannot be a
// fragment-stage output.  Declaring it on a fragment output is illegal.
struct PSOut {
    float4 color : SV_Target0;
    float  clip  : SV_ClipDistance;
};

PSOut main()
{
    PSOut outp;
    outp.color = float4(1.0, 1.0, 1.0, 1.0);
    outp.clip = 1.0;
    return outp;
}
